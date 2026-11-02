// Measures TempConflictBTree at the block size it is compiled with
// (-DRESOLVER1_BLOCK_SIZE), with the same budget in bytes for every size and
// no reclamation. The batches are made before timing, from a fixed seed, so
// that every build sees the same ones.
//
// One source for every tree, through the interface its tree has, chosen with
// -DBENCH_API (3 by default):
// - 1, btree-v1: checkReads(reads, status) and addWrites(sorted writes, cv).
// - 2, btree-v2: checkReads<G> over the sorted endpoints of the reads, then
//   addWrites(sorted writes, cv).
// - 3, btree-v3: checkReads<G> over the sorted endpoints of the reads and the
//   writes, which also prepares the writes' destinations, then
//   addWrites(points, transactions, final decisions, cv).
// G is -DBENCH_TASKS, 16 by default.
//
// Each batch is timed in three consecutive, non-overlapping phases, and as a
// whole: prepS builds the inputs the tree's interface takes from the batch
// (for 2 and 3 the endpoints, sorted, and for 3 each endpoint's position in
// its slot); readS is checkReads; writeS is addWrites; totalS is the three
// together. With 3, part of the work on writes is in checkReads, so only
// totalS compares the trees as a whole.
//
// Each read is a transaction of its own, so its decision is the read's; the
// writes are one more transaction without reads, last, which commits.
//
// Usage: blocksizebench <points|ranges> <batches> <budget MiB>
// Prints one line: interface, G, block size, capacities, seconds and rates,
// the reclaim cut (0 when nothing was reclaimed) and a hash of the read
// decisions.
#include "tempconflictbtree.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

struct Random {
	std::uint64_t state;
	explicit Random(std::uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
	std::uint64_t next() {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	}
	std::uint64_t below(std::uint64_t n) { return next() % n; }
};

// 12-byte keys: a prefix and a big-endian number, so they sort as numbers.
constexpr int KeyBytes = 12;
constexpr std::uint64_t KeySpace = 10'000'000;

#ifndef BENCH_API
#define BENCH_API 3
#endif
#ifndef BENCH_TASKS
#define BENCH_TASKS 16
#endif
// Tasks interleaved by checkReads; tempconflictbtree.cpp instantiates it.
constexpr std::uint8_t Tasks = BENCH_TASKS;

struct Batch {
	Version cv;
	std::vector<std::pair<int, int>> writes;          // indices into keys
	std::vector<std::pair<int, int>> reads;
	std::vector<Version> readVersions;
};

}  // namespace

int main(int argc, char** argv) {
	if (argc != 4) {
		std::fprintf(stderr, "usage: %s <points|ranges> <batches> <budget MiB>\n", argv[0]);
		return 2;
	}
	const bool ranges = std::strcmp(argv[1], "ranges") == 0;
	const int batches = std::atoi(argv[2]);
	const std::size_t budget = std::size_t(std::atoll(argv[3])) << 20;
	const int writesPerBatch = 1000, readsPerBatch = 4000;

	// Key bytes for every endpoint, made before timing.
	std::vector<std::uint8_t> keyBytes;
	auto addKey = [&keyBytes](std::uint64_t n) {
		const int index = int(keyBytes.size() / KeyBytes);
		keyBytes.push_back('k');
		keyBytes.push_back('/');
		keyBytes.push_back('x');
		keyBytes.push_back('/');
		for (int b = 7; b >= 0; --b) {
			keyBytes.push_back(std::uint8_t(n >> (8 * b)));
		}
		return index;
	};

	Random rng(1234);
	std::vector<Batch> work(batches);
	Version cv = 1000;
	for (Batch& batch : work) {
		batch.cv = cv;
		// Writes sorted and disjoint, as the resolver hands them over.
		std::vector<std::uint64_t> starts;
		for (int i = 0; i < writesPerBatch; ++i) {
			starts.push_back(rng.below(KeySpace));
		}
		std::sort(starts.begin(), starts.end());
		std::uint64_t floor = 0;
		for (std::uint64_t s : starts) {
			if (s < floor) {
				continue;
			}
			const std::uint64_t length = ranges ? 1 + rng.below(rng.below(10) == 0 ? 2000 : 20) : 1;
			batch.writes.emplace_back(addKey(s), addKey(s + length));
			floor = s + length;
		}
		for (int i = 0; i < readsPerBatch; ++i) {
			const std::uint64_t s = rng.below(KeySpace);
			const std::uint64_t length = ranges ? 1 + rng.below(rng.below(10) == 0 ? 2000 : 20) : 1;
			batch.reads.emplace_back(addKey(s), addKey(s + length));
			batch.readVersions.push_back(cv - 1 - Version(rng.below(500)));
		}
		cv += 10;
	}

	auto key = [&keyBytes](int index) { return StringRef(keyBytes.data() + std::size_t(index) * KeyBytes, KeyBytes); };

	TempConflictBTree tree(budget);
	std::vector<std::pair<StringRef, StringRef>> writes;
	std::vector<ReadConflictRange> reads;
	// One transaction per read, and a last one with the writes, so status
	// holds the decision of each read. Each read transaction's only read range
	// is its entry in readRanges, which its endpoints point into, as in FDB's
	// batch; the writer's ranges are in writeSlots.
	const int transactionCount = readsPerBatch + 1;
	const int writer = readsPerBatch;
	std::vector<std::uint8_t> status(transactionCount);
#if BENCH_API != 1
	std::vector<std::pair<int, int>> readRanges(readsPerBatch);
	std::vector<std::pair<int, int>> writeSlots;
	writeSlots.reserve(writesPerBatch);
	std::vector<TransactionInfo> transactions(transactionCount);
	std::vector<TransactionInfo*> transactionInfo(transactionCount);
	std::vector<int> offsets(transactionCount, 0);
	std::vector<KeyInfo> points;
	std::vector<std::uint8_t> readConflicts;
	for (int i = 0; i < transactionCount; ++i) {
		if (i < readsPerBatch) {
			transactions[i].readRanges = VectorRef<std::pair<int, int>>(&readRanges[i], 1);
			offsets[i] = i;
		}
		transactions[i].tooOld = false;
		transactions[i].reportConflictingKeys = false;
		transactionInfo[i] = &transactions[i];
	}
#endif
	using Clock = std::chrono::steady_clock;
	auto seconds = [](Clock::time_point a, Clock::time_point b) {
		return std::chrono::duration<double>(b - a).count();
	};
	double prepSeconds = 0, readSeconds = 0, writeSeconds = 0, totalSeconds = 0;
	std::uint64_t hash = 1469598103934665603ull, conflicts = 0, writeRanges = 0;
	for (const Batch& batch : work) {
		const auto t0 = Clock::now();
		// Preparation: what the interface takes from the batch.
		reads.clear();
		for (std::size_t i = 0; i < batch.reads.size(); ++i) {
			reads.emplace_back(key(batch.reads[i].first), key(batch.reads[i].second),
			                   batch.readVersions[i], int(i), 0);
		}
		std::memset(status.data(), 0, status.size());
#if BENCH_API == 1 || BENCH_API == 2
		writes.clear();
		for (const auto& [b, e] : batch.writes) {
			writes.emplace_back(key(b), key(e));
		}
#endif
#if BENCH_API == 2 || BENCH_API == 3
		points.clear();
		for (std::size_t i = 0; i < batch.reads.size(); ++i) {
			points.emplace_back(reads[i].begin, true, false, int(i), &readRanges[i].first);
			points.emplace_back(reads[i].end, false, false, int(i), &readRanges[i].second);
		}
#endif
#if BENCH_API == 3
		writeSlots.resize(batch.writes.size());
		transactions[writer].writeRanges =
		    VectorRef<std::pair<int, int>>(writeSlots.data(), int(writeSlots.size()));
		for (std::size_t w = 0; w < batch.writes.size(); ++w) {
			points.emplace_back(key(batch.writes[w].first), true, true, writer, &writeSlots[w].first);
			points.emplace_back(key(batch.writes[w].second), false, true, writer, &writeSlots[w].second);
		}
#endif
#if BENCH_API == 2 || BENCH_API == 3
		std::stable_sort(points.begin(), points.end(), [](const KeyInfo& a, const KeyInfo& b) {
			return a.key.compare(b.key) < 0;
		});
#endif
#if BENCH_API == 3
		for (std::size_t p = 0; p < points.size(); ++p) {
			*points[p].pIndex = int(p);
		}
#endif
		[[maybe_unused]] int64_t tooOld = 0;
		const auto t1 = Clock::now();
#if BENCH_API == 1
		tree.checkReads(std::span<const ReadConflictRange>(reads.data(), reads.size()),
		                reinterpret_cast<bool*>(status.data()));
#else
		tree.checkReads<Tasks>(points, transactionInfo.data(), reads, offsets,
		                       reinterpret_cast<bool*>(status.data()), readConflicts, tooOld);
#endif
		const auto t2 = Clock::now();
#if BENCH_API == 3
		tree.addWrites(points, transactionInfo.data(), reinterpret_cast<const bool*>(status.data()), batch.cv);
#else
		tree.addWrites(std::span<const std::pair<StringRef, StringRef>>(writes.data(), writes.size()), batch.cv);
#endif
		const auto t3 = Clock::now();
		prepSeconds += seconds(t0, t1);
		readSeconds += seconds(t1, t2);
		writeSeconds += seconds(t2, t3);
		totalSeconds += seconds(t0, t3);
		writeRanges += batch.writes.size();
		for (int i = 0; i < readsPerBatch; ++i) {
			conflicts += status[i];
			hash = (hash ^ status[i]) * 1099511628211ull;
		}
		if (status[writer] != 0 || tooOld != 0) {
			std::fprintf(stderr, "the writer conflicted or a read was too old\n");
			return 1;
		}
	}
	const double readCount = double(batches) * readsPerBatch;
	std::printf("api=%d tasks=%d block=%zu internal=%zu leaf=%zu workload=%s batches=%d reads=%.0f "
	            "writes=%llu prepS=%.3f readS=%.3f writeS=%.3f totalS=%.3f batchesPerS=%.1f readsPerS=%.0f "
	            "writesPerS=%.0f conflicts=%llu reclaimCV=%lld decisions=%016llx\n",
	            BENCH_API, BENCH_API == 1 ? 0 : int(Tasks), BlockSize, InternalCapacity, LeafCapacity,
	            ranges ? "ranges" : "points", batches, readCount, (unsigned long long)writeRanges, prepSeconds,
	            readSeconds, writeSeconds, totalSeconds, batches / totalSeconds, readCount / readSeconds,
	            double(writeRanges) / writeSeconds, (unsigned long long)conflicts, (long long)tree.reclaimCV(),
	            (unsigned long long)hash);
	return tree.reclaimCV() == 0 ? 0 : 1;
}
