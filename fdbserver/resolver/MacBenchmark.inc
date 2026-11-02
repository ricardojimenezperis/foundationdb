/*
 * MacBenchmark.inc
 *
 * resolver1's own benchmark, bench/blocksizebench.cpp (the "Mac benchmark"), through the Resolver's
 * batch path, so that every history a binary can select runs it: FDB's skip list and v107 by their
 * knobs, btree-v1 and btree-v2.5 in their own binaries. Included at the end of ConflictSet.cpp.
 *
 * The batches are the benchmark's, drawn from the same generator and seed before anything is timed:
 * 12-byte keys over 10^7 values; per batch 1000 sorted, disjoint writes and 4000 reads, of one key
 * (points) or of 1 to 20 keys, one in ten of 1 to 2000 (ranges), at read versions up to 500 below
 * the batch's. Each read is a transaction of its own, so its decision is the read's; the writes are
 * one last transaction without reads, which commits and, being last, conflicts with no read of its
 * batch. No version leaves the window, so nothing is too old and the skip list removes nothing.
 *
 * Reads are timed as the batch path's read check (D.CheckRead) and writes as its merge (D.MergeWrite),
 * the phases blocksizebench times on the tree. Each batch is also timed whole (totalS: adding its
 * transactions and detectConflicts), and in the phases of the batch path, consecutive and without
 * overlap: adding the transactions (addS), sorting the endpoints (sortS), the read check, the
 * intra-batch check (intraBatchS), combining the writes (combineS), the merge and the cleanup
 * (cleanupS). An index that moves work between phases, as btree-v3 moves part of the writes into
 * its read check, is compared by totalS. Decisions are hashed as blocksizebench hashes them:
 * points 4787dbf4af9b9ca8 and ranges df42a6e5c89796ea with 3000 batches.
 */

namespace {

struct MacBenchmarkRandom {
	uint64_t state;
	explicit MacBenchmarkRandom(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
	uint64_t next() {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	}
	uint64_t below(uint64_t n) { return next() % n; }
};

struct MacBenchmarkBatch {
	Version cv;
	std::vector<std::pair<uint64_t, uint64_t>> writes;
	std::vector<std::pair<uint64_t, uint64_t>> reads;
	std::vector<Version> readVersions;
};

StringRef macBenchmarkKey(Arena& arena, uint64_t n) {
	uint8_t* bytes = new (arena) uint8_t[12];
	bytes[0] = 'k';
	bytes[1] = '/';
	bytes[2] = 'x';
	bytes[3] = '/';
	for (int b = 7; b >= 0; --b) {
		bytes[4 + (7 - b)] = uint8_t(n >> (8 * b));
	}
	return StringRef(bytes, 12);
}

} // namespace

TEST_CASE("noSim/fdbserver/conflictset/macBenchmark") {
	const std::string workload = params.get("workload").orDefault("points");
	const int batches = params.getInt("batches").orDefault(3000);
	const bool ranges = workload == "ranges";
	ASSERT(ranges || workload == "points");
	constexpr uint64_t KeySpace = 10'000'000;
	const int writesPerBatch = 1000, readsPerBatch = 4000;

	// The batches, made before anything is timed, in blocksizebench's order of draws.
	MacBenchmarkRandom rng(1234);
	std::vector<MacBenchmarkBatch> work(batches);
	Version cv = 1000;
	for (MacBenchmarkBatch& batch : work) {
		batch.cv = cv;
		std::vector<uint64_t> starts;
		for (int i = 0; i < writesPerBatch; ++i) {
			starts.push_back(rng.below(KeySpace));
		}
		std::sort(starts.begin(), starts.end());
		uint64_t floor = 0;
		for (uint64_t s : starts) {
			if (s < floor) {
				continue;
			}
			const uint64_t length = ranges ? 1 + rng.below(rng.below(10) == 0 ? 2000 : 20) : 1;
			batch.writes.emplace_back(s, s + length);
			floor = s + length;
		}
		for (int i = 0; i < readsPerBatch; ++i) {
			const uint64_t s = rng.below(KeySpace);
			const uint64_t length = ranges ? 1 + rng.below(rng.below(10) == 0 ? 2000 : 20) : 1;
			batch.reads.emplace_back(s, s + length);
			batch.readVersions.push_back(cv - 1 - Version(rng.below(500)));
		}
		cv += 10;
	}

	ConflictSet* cs = newConflictSet();
	const ConflictSetStats atStart = getConflictSetStats(cs);
	const double sortAtStart = g_sort.getValue();
	double totalSeconds = 0, addSeconds = 0;
	uint64_t hash = 1469598103934665603ull, conflicts = 0, writeRanges = 0;
	for (const MacBenchmarkBatch& batch : work) {
		Arena arena;
		std::vector<CommitTransactionRef> trs(batch.reads.size() + 1);
		for (size_t i = 0; i < batch.reads.size(); ++i) {
			trs[i].read_snapshot = batch.readVersions[i];
			trs[i].read_conflict_ranges.push_back(
			    arena,
			    KeyRangeRef(macBenchmarkKey(arena, batch.reads[i].first), macBenchmarkKey(arena, batch.reads[i].second)));
		}
		CommitTransactionRef& writer = trs.back();
		writer.read_snapshot = batch.cv - 1;
		for (const auto& [b, e] : batch.writes) {
			writer.write_conflict_ranges.push_back(arena,
			                                       KeyRangeRef(macBenchmarkKey(arena, b), macBenchmarkKey(arena, e)));
		}
		writeRanges += batch.writes.size();

		std::vector<int> nonConflicting, tooOld;
		{
			const double start = timer_monotonic();
			ConflictBatch conflictBatch(cs);
			for (const CommitTransactionRef& tr : trs) {
				conflictBatch.addTransaction(tr, 0);
			}
			const double added = timer_monotonic();
			conflictBatch.detectConflicts(batch.cv, 0, nonConflicting, &tooOld);
			const double end = timer_monotonic();
			addSeconds += added - start;
			totalSeconds += end - start;
		}
		ASSERT(tooOld.empty());
		std::vector<uint8_t> status(trs.size(), 1);
		for (int t : nonConflicting) {
			status[t] = 0;
		}
		ASSERT(status.back() == 0);
		for (size_t i = 0; i < batch.reads.size(); ++i) {
			conflicts += status[i];
			hash = (hash ^ status[i]) * 1099511628211ull;
		}
	}
	const ConflictSetStats atEnd = getConflictSetStats(cs);
	const double readSeconds = atEnd.checkReadSeconds - atStart.checkReadSeconds;
	const double writeSeconds = atEnd.mergeSeconds - atStart.mergeSeconds;
	const double readCount = double(batches) * readsPerBatch;
	printf("MacBenchmarkPhases index=%s workload=%s batches=%d totalS=%.3f addS=%.3f sortS=%.3f readS=%.3f "
	       "intraBatchS=%.3f combineS=%.3f writeS=%.3f cleanupS=%.3f batchesPerS=%.1f\n",
	       conflictSetIndexName(cs),
	       workload.c_str(),
	       batches,
	       totalSeconds,
	       addSeconds,
	       g_sort.getValue() - sortAtStart,
	       readSeconds,
	       atEnd.checkIntraBatchSeconds - atStart.checkIntraBatchSeconds,
	       atEnd.combineSeconds - atStart.combineSeconds,
	       writeSeconds,
	       atEnd.cleanupSeconds - atStart.cleanupSeconds,
	       batches / totalSeconds);
	printf("MacBenchmark index=%s workload=%s batches=%d reads=%.0f writes=%llu readS=%.3f writeS=%.3f "
	       "combineS=%.3f readsPerS=%.0f writesPerS=%.0f conflicts=%llu decisions=%016llx\n",
	       conflictSetIndexName(cs),
	       workload.c_str(),
	       batches,
	       readCount,
	       (unsigned long long)writeRanges,
	       readSeconds,
	       writeSeconds,
	       atEnd.combineSeconds - atStart.combineSeconds,
	       readCount / readSeconds,
	       double(writeRanges) / writeSeconds,
	       (unsigned long long)conflicts,
	       (unsigned long long)hash);
	destroyConflictSet(cs);
	return Void();
}
