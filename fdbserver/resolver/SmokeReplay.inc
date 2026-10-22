/*
 * SmokeReplay.inc
 *
 * The btree-v1 smoke test's batch replayer. It is included at the end of ConflictSet.cpp, in
 * the same form in every binary compared, and runs only as a unit test: it is outside the
 * resolver's normal path. It resolves deterministic batches, made from fixed seeds, through
 * newConflictSet(), so through whichever history this binary and its knobs select, and writes
 * one line per batch: what was offered, its decisions and the conflicting ranges reported.
 * Comparing those files across binaries compares the histories on the same batches.
 *
 * Environment:
 *   SMOKE_OUT      output file (required)
 *   SMOKE_PAD      bytes appended to every key (default 0)
 *   SMOKE_SEED     seed of the first case; case i uses SMOKE_SEED + i (default 1001)
 *   SMOKE_SCALE    multiplies the number of batches of every case (default 1)
 *   SMOKE_EXCLUDE  an earlier output: the transactions it reports as too old are replaced,
 *                  batch by batch, by transactions without ranges
 *   SMOKE_DUMP     "case:batch": also writes that batch's transactions, readable
 */

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

struct SmokeRandom {
	uint64_t state;
	explicit SmokeRandom(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
	uint64_t next() {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	}
	int below(int n) { return static_cast<int>(next() % static_cast<uint64_t>(n)); }
};

std::string smokeKey(int n, int pad) {
	return format("k%08d", n) + std::string(pad, '.');
}

void smokePoint(std::vector<std::pair<std::string, std::string>>& ranges, const std::string& key) {
	ranges.emplace_back(key, keyAfterOf(key));
}

void smokeRange(std::vector<std::pair<std::string, std::string>>& ranges, int from, int to, int pad) {
	ranges.emplace_back(smokeKey(from, pad), smokeKey(to, pad));
}

// Mostly inside the window, sometimes just outside it; a third report conflicting ranges
TransactionSpec smokeTransaction(SmokeRandom& rng, Version now) {
	TransactionSpec t;
	t.snapshot = now - 1 - rng.below(testWindow + testWindow / 5);
	t.report = rng.below(3) == 0;
	return t;
}

struct SmokeCase {
	const char* name;
	int batches;
	std::function<std::vector<TransactionSpec>(SmokeRandom&, Version, int)> make;
};

std::vector<SmokeCase> smokeCases(int pad, int scale) {
	return {
		{ "scattered-points",
		  300 * scale,
		  [pad](SmokeRandom& rng, Version now, int) {
		      std::vector<TransactionSpec> batch;
		      for (int i = 0; i < 20; ++i) {
			      TransactionSpec t = smokeTransaction(rng, now);
			      for (int r = rng.below(4); r > 0; --r)
				      smokePoint(t.reads, smokeKey(rng.below(100000), pad));
			      for (int w = rng.below(3); w > 0; --w)
				      smokePoint(t.writes, smokeKey(rng.below(100000), pad));
			      batch.push_back(t);
		      }
		      return batch;
		  } },
		{ "short-overlapping-ranges",
		  300 * scale,
		  [pad](SmokeRandom& rng, Version now, int) {
		      std::vector<TransactionSpec> batch;
		      for (int i = 0; i < 20; ++i) {
			      TransactionSpec t = smokeTransaction(rng, now);
			      for (int r = rng.below(3); r > 0; --r) {
				      const int k = rng.below(2000);
				      smokeRange(t.reads, k, k + 1 + rng.below(5), pad);
			      }
			      for (int w = rng.below(3); w > 0; --w) {
				      const int k = rng.below(2000);
				      smokeRange(t.writes, k, k + 1 + rng.below(5), pad);
			      }
			      batch.push_back(t);
		      }
		      return batch;
		  } },
		{ "long-ranges",
		  300 * scale,
		  [pad](SmokeRandom& rng, Version now, int b) {
		      std::vector<TransactionSpec> batch;
		      for (int i = 0; i < 20; ++i) {
			      TransactionSpec t = smokeTransaction(rng, now);
			      if (b % 300 < 100) {
				      // Points first, so that the ranges after them cross many leaves
				      for (int w = 0; w < 2; ++w)
					      smokePoint(t.writes, smokeKey(rng.below(20000), pad));
			      } else {
				      const int k = rng.below(20000);
				      smokeRange(t.reads, k, k + 200 + rng.below(3000), pad);
				      if (rng.below(2) == 0) {
					      const int w = rng.below(20000);
					      smokeRange(t.writes, w, w + 200 + rng.below(3000), pad);
				      }
			      }
			      batch.push_back(t);
		      }
		      return batch;
		  } },
		{ "write-inside-range",
		  300 * scale,
		  [pad](SmokeRandom& rng, Version now, int b) {
		      std::vector<TransactionSpec> batch;
		      const int base = (b / 10) * 1000 % 50000;
		      for (int i = 0; i < 20; ++i) {
			      TransactionSpec t = smokeTransaction(rng, now);
			      if (b % 10 == 0 && i == 0) {
				      // A long range, older than what is then written inside it
				      smokeRange(t.writes, base, base + 1000, pad);
			      } else {
				      const int x = base + 1 + rng.below(997);
				      smokeRange(t.writes, x, x + 1 + rng.below(2), pad);
				      // Reads on both exteriors and on the part written inside
				      smokeRange(t.reads, base, base + 1 + rng.below(3), pad);
				      smokeRange(t.reads, base + 995 + rng.below(4), base + 1000, pad);
				      smokePoint(t.reads, smokeKey(x, pad));
			      }
			      batch.push_back(t);
		      }
		      return batch;
		  } },
		{ "growth",
		  200 * scale,
		  [pad](SmokeRandom& rng, Version now, int b) {
		      std::vector<TransactionSpec> batch;
		      for (int i = 0; i < 20; ++i) {
			      TransactionSpec t = smokeTransaction(rng, now);
			      // Fresh keys spread over the key space: leaves, internal nodes and the root split
			      for (int w = 0; w < 3; ++w)
				      smokePoint(t.writes,
				                 smokeKey(static_cast<int>((int64_t(b * 60 + i * 3 + w) * 7919) % 1000003), pad));
			      smokePoint(t.reads, smokeKey(rng.below(1000003), pad));
			      batch.push_back(t);
		      }
		      return batch;
		  } },
		{ "erasures",
		  300 * scale,
		  [pad](SmokeRandom& rng, Version now, int b) {
		      std::vector<TransactionSpec> batch;
		      for (int i = 0; i < 20; ++i) {
			      TransactionSpec t = smokeTransaction(rng, now);
			      if (b % 300 < 150) {
				      for (int w = 0; w < 2; ++w)
					      smokePoint(t.writes, smokeKey(rng.below(100000), pad));
			      } else if (i == 0) {
				      // Wide writes erase what they cover: leaves empty and levels go
				      const int k = rng.below(80000);
				      smokeRange(t.writes, k, k + 20000, pad);
			      } else {
				      const int k = rng.below(100000);
				      smokeRange(t.reads, k, k + 1 + rng.below(50), pad);
			      }
			      batch.push_back(t);
		      }
		      return batch;
		  } },
	};
}

void smokeHash(uint64_t& h, uint64_t value) {
	for (int b = 0; b < 8; ++b) {
		h = (h ^ ((value >> (8 * b)) & 0xFF)) * 1099511628211ULL;
	}
}

void smokeHash(uint64_t& h, const std::string& s) {
	smokeHash(h, s.size());
	for (unsigned char c : s) {
		h = (h ^ c) * 1099511628211ULL;
	}
}

uint64_t smokeBatchHash(const std::vector<TransactionSpec>& batch) {
	uint64_t h = 1469598103934665603ULL;
	for (const TransactionSpec& t : batch) {
		smokeHash(h, uint64_t(t.snapshot));
		smokeHash(h, uint64_t(t.report));
		for (const auto* ranges : { &t.reads, &t.writes }) {
			smokeHash(h, ranges->size());
			for (const auto& [from, to] : *ranges) {
				smokeHash(h, from);
				smokeHash(h, to);
			}
		}
	}
	return h;
}

// Keys as written by smokeKey, with the padding and a trailing zero byte made readable
std::string smokeReadable(const std::string& key) {
	std::string out;
	size_t dots = 0;
	for (unsigned char c : key) {
		if (c == '.') {
			++dots;
			continue;
		}
		out += c == 0 ? std::string("\\0") : std::string(1, char(c));
	}
	if (dots) {
		out += format("+%zu.", dots);
	}
	return out;
}

std::string smokeEnv(const char* name, const char* fallback) {
	const char* v = std::getenv(name);
	return v && *v ? std::string(v) : std::string(fallback);
}

// Too-old transactions per case and batch, from the "batch" lines of an earlier output
std::map<std::pair<std::string, int>, std::vector<int>> smokeReadExclusions(const std::string& path) {
	std::map<std::pair<std::string, int>, std::vector<int>> excluded;
	std::ifstream in(path);
	ASSERT(in.good());
	std::string line;
	while (std::getline(in, line)) {
		std::vector<std::string> f;
		size_t start = 0;
		for (size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1) {
			f.push_back(line.substr(start, tab - start));
		}
		f.push_back(line.substr(start));
		if (f.size() < 8 || f[0] != "batch") {
			continue;
		}
		std::vector<int>& list = excluded[{ f[1], std::stoi(f[2]) }];
		for (size_t t = 0; t < f[7].size(); ++t) {
			if (f[7][t] == 'T') {
				list.push_back(int(t));
			}
		}
	}
	return excluded;
}

} // namespace

TEST_CASE("noSim/fdbserver/conflictset/smokeReplay") {
	const std::string outPath = smokeEnv("SMOKE_OUT", "");
	ASSERT(!outPath.empty());
	const int pad = std::stoi(smokeEnv("SMOKE_PAD", "0"));
	const uint64_t firstSeed = std::stoull(smokeEnv("SMOKE_SEED", "1001"));
	const int scale = std::stoi(smokeEnv("SMOKE_SCALE", "1"));
	const std::string excludePath = smokeEnv("SMOKE_EXCLUDE", "");
	const std::string dump = smokeEnv("SMOKE_DUMP", "");
	const auto excluded = excludePath.empty() ? std::map<std::pair<std::string, int>, std::vector<int>>()
	                                          : smokeReadExclusions(excludePath);

	FILE* out = fopen(outPath.c_str(), "w");
	ASSERT(out != nullptr);
	uint64_t seed = firstSeed;
	for (const SmokeCase& c : smokeCases(pad, scale)) {
		ConflictSet* cs = newConflictSet();
		SmokeRandom rng(seed);
		Version now = 2000;
		int64_t transactions = 0, reads = 0, writes = 0, tooOld = 0;
		double seconds = 0;
		for (int b = 0; b < c.batches; ++b, now += 10) {
			std::vector<TransactionSpec> batch = c.make(rng, now, b);
			const uint64_t offered = smokeBatchHash(batch);
			if (dump == format("%s:%d", c.name, b)) {
				for (size_t t = 0; t < batch.size(); ++t) {
					fprintf(out, "dump\t%s\t%d\t%zu\tsnapshot=%lld report=%d", c.name, b, t, (long long)batch[t].snapshot, int(batch[t].report));
					for (const auto& [from, to] : batch[t].reads)
						fprintf(out, " read=[%s,%s)", smokeReadable(from).c_str(), smokeReadable(to).c_str());
					for (const auto& [from, to] : batch[t].writes)
						fprintf(out, " write=[%s,%s)", smokeReadable(from).c_str(), smokeReadable(to).c_str());
					fprintf(out, "\n");
				}
			}
			if (auto it = excluded.find({ c.name, b }); it != excluded.end()) {
				for (int t : it->second) {
					batch[t] = TransactionSpec{ batch[t].snapshot, {}, {}, false };
				}
			}
			const Version boundary = getConflictSetStats(cs).validationMinRV;
			ResolvedBatch result;
			const double start = timer();
			resolve(cs, now, batch, result);
			seconds += timer() - start;

			std::string decisions, snapshots, reported;
			for (int t = 0; t < (int)batch.size(); ++t) {
				transactions += 1;
				reads += batch[t].reads.size();
				writes += batch[t].writes.size();
				decisions += result.isTooOld(t) ? 'T' : result.isCommitted(t) ? 'C' : 'X';
				snapshots += format("%s%lld%s", t ? "," : "", (long long)batch[t].snapshot, batch[t].reads.empty() ? "" : "r");
				if (!result.reported(t).empty()) {
					reported += format("%s%d:", reported.empty() ? "" : ";", t);
					for (size_t i = 0; i < result.reported(t).size(); ++i)
						reported += format("%s%d", i ? "," : "", result.reported(t)[i]);
				}
			}
			tooOld += result.tooOld.size();
			fprintf(out,
			        "batch\t%s\t%d\t%lld\t%lld\t%lld\t%016llx\t%s\t%s\t%s\n",
			        c.name,
			        b,
			        (long long)now,
			        (long long)(now - testWindow),
			        (long long)boundary,
			        (unsigned long long)offered,
			        decisions.c_str(),
			        reported.empty() ? "-" : reported.c_str(),
			        snapshots.c_str());
		}
		fprintf(out,
		        "summary\t%s\t%s\t%llu\t%d\t%lld\t%lld\t%lld\t%lld\t%lld\t%.6f\n",
		        c.name,
		        conflictSetIndexName(cs),
		        (unsigned long long)seed,
		        c.batches,
		        (long long)transactions,
		        (long long)reads,
		        (long long)writes,
		        (long long)tooOld,
		        (long long)getConflictSetStats(cs).validationMinRV,
		        seconds);
		destroyConflictSet(cs);
		++seed;
	}
	fclose(out);
	return Void();
}
