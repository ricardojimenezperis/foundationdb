// Measures TempConflictBTree at the block size it is compiled with
// (-DRESOLVER1_BLOCK_SIZE), with the same budget in bytes for every size and
// no reclamation. The batches are made before timing, from a fixed seed, so
// that every build sees the same ones. Reads and writes are timed apart.
//
// Usage: blocksizebench <points|ranges> <batches> <budget MiB>
// Prints one line: block size, capacities, seconds and rates, the reclaim
// cut (0 when nothing was reclaimed) and a hash of the read decisions.
#include "tempconflictbtree.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// Tasks interleaved by checkReads; tempconflictbtree.cpp instantiates it.
constexpr std::uint8_t Tasks = 16;

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
	// One transaction per read, so status holds the decision of each read.
	// Each transaction's only read range is its entry in readRanges, which
	// its endpoints point into, as in FDB's batch.
	std::vector<std::uint8_t> status(readsPerBatch);
	std::vector<std::pair<int, int>> readRanges(readsPerBatch);
	std::vector<TransactionInfo> transactions(readsPerBatch);
	std::vector<TransactionInfo*> transactionInfo(readsPerBatch);
	std::vector<int> offsets(readsPerBatch);
	std::vector<KeyInfo> points;
	std::vector<std::uint8_t> readConflicts;
	for (int i = 0; i < readsPerBatch; ++i) {
		transactions[i].readRanges = VectorRef<std::pair<int, int>>(&readRanges[i], 1);
		transactionInfo[i] = &transactions[i];
		offsets[i] = i;
	}
	double readSeconds = 0, writeSeconds = 0;
	std::uint64_t hash = 1469598103934665603ull, conflicts = 0, writeRanges = 0;
	for (const Batch& batch : work) {
		reads.clear();
		points.clear();
		for (std::size_t i = 0; i < batch.reads.size(); ++i) {
			const StringRef begin = key(batch.reads[i].first);
			const StringRef end = key(batch.reads[i].second);
			reads.emplace_back(begin, end, batch.readVersions[i], int(i), 0);
			points.emplace_back(begin, true, false, int(i), &readRanges[i].first);
			points.emplace_back(end, false, false, int(i), &readRanges[i].second);
			transactions[i].tooOld = false;
			transactions[i].reportConflictingKeys = false;
		}
		std::stable_sort(points.begin(), points.end(), [](const KeyInfo& a, const KeyInfo& b) {
			return a.key.compare(b.key) < 0;
		});
		std::memset(status.data(), 0, status.size());
		int64_t tooOld = 0;
		auto t0 = std::chrono::steady_clock::now();
		tree.checkReads<Tasks>(points, transactionInfo.data(), reads, offsets,
		                       reinterpret_cast<bool*>(status.data()), readConflicts, tooOld);
		auto t1 = std::chrono::steady_clock::now();
		writes.clear();
		for (const auto& [b, e] : batch.writes) {
			writes.emplace_back(key(b), key(e));
		}
		auto t2 = std::chrono::steady_clock::now();
		tree.addWrites(std::span<const std::pair<StringRef, StringRef>>(writes.data(), writes.size()), batch.cv);
		auto t3 = std::chrono::steady_clock::now();
		readSeconds += std::chrono::duration<double>(t1 - t0).count();
		writeSeconds += std::chrono::duration<double>(t3 - t2).count();
		writeRanges += writes.size();
		for (std::uint8_t s : status) {
			conflicts += s;
			hash = (hash ^ s) * 1099511628211ull;
		}
	}
	const double readCount = double(batches) * readsPerBatch;
	std::printf("block=%zu internal=%zu leaf=%zu workload=%s batches=%d reads=%.0f writes=%llu "
	            "readS=%.3f writeS=%.3f totalS=%.3f readsPerS=%.0f writesPerS=%.0f conflicts=%llu "
	            "reclaimCV=%lld decisions=%016llx\n",
	            BlockSize, InternalCapacity, LeafCapacity, ranges ? "ranges" : "points", batches, readCount,
	            (unsigned long long)writeRanges, readSeconds, writeSeconds, readSeconds + writeSeconds,
	            readCount / readSeconds, double(writeRanges) / writeSeconds, (unsigned long long)conflicts,
	            (long long)tree.reclaimCV(), (unsigned long long)hash);
	return tree.reclaimCV() == 0 ? 0 : 1;
}
