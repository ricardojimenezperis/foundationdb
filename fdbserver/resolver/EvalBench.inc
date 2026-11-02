#include "fdbserver/core/Knobs.h"

namespace {

// Seconds spent adding the batch's transactions, the part of a batch before detectConflicts
double evalAddSeconds = 0;

StringRef evalBenchmarkKey(Arena& arena, int64_t n) {
	uint8_t* bytes = new (arena) uint8_t[10];
	bytes[0] = 'c';
	bytes[1] = '/';
	for (int i = 0; i < 8; ++i) {
		bytes[2 + i] = uint8_t(uint64_t(n) >> (56 - 8 * i));
	}
	return StringRef(bytes, 10);
}

force_inline void evalResolveBatch(ConflictSet* cs,
                                   const std::vector<CommitTransactionRef>& trs,
                                   Version now,
                                   Version newOldestVersion,
                                   std::vector<int>& nonConflicting,
                                   std::vector<int>& tooOldList,
                                   std::map<int, VectorRef<int>>& conflictingKeys,
                                   Arena& replyArena) {
	ConflictBatch batch(cs, &conflictingKeys, &replyArena);
	const double start = timer_monotonic();
	for (const CommitTransactionRef& tr : trs) {
		batch.addTransaction(tr, newOldestVersion);
	}
	evalAddSeconds += timer_monotonic() - start;
	batch.detectConflicts(now, newOldestVersion, nonConflicting, &tooOldList);
}

__attribute__((noinline)) void evalWarmupBatch(ConflictSet* cs,
                                               const std::vector<CommitTransactionRef>& trs,
                                               Version now,
                                               Version newOldestVersion,
                                               std::vector<int>& nonConflicting,
                                               std::vector<int>& tooOldList,
                                               std::map<int, VectorRef<int>>& conflictingKeys,
                                               Arena& replyArena) {
	evalResolveBatch(cs, trs, now, newOldestVersion, nonConflicting, tooOldList, conflictingKeys, replyArena);
}

__attribute__((noinline)) void evalMeasuredBatch(ConflictSet* cs,
                                                 const std::vector<CommitTransactionRef>& trs,
                                                 Version now,
                                                 Version newOldestVersion,
                                                 std::vector<int>& nonConflicting,
                                                 std::vector<int>& tooOldList,
                                                 std::map<int, VectorRef<int>>& conflictingKeys,
                                                 Arena& replyArena) {
	evalResolveBatch(cs, trs, now, newOldestVersion, nonConflicting, tooOldList, conflictingKeys, replyArena);
}

double evalPercentile(std::vector<double> samples, double p) {
	if (samples.empty()) {
		return 0;
	}
	std::sort(samples.begin(), samples.end());
	return samples[std::min(samples.size() - 1, size_t(p * double(samples.size())))];
}

} // namespace

TEST_CASE("noSim/fdbserver/conflictset/benchmark") {
	const std::string workload = params.get("workload").orDefault("points");
	const int warmupBatches = params.getInt("warmupBatches").orDefault(1000);
	const int batches = params.getInt("batches").orDefault(2000);
	const int transactionsPerBatch = params.getInt("transactionsPerBatch").orDefault(500);
	const int readsPerTransaction = params.getInt("readsPerTransaction").orDefault(4);
	const int writesPerTransaction = params.getInt("writesPerTransaction").orDefault(2);
	const int64_t keySpace = params.getInt("keySpace").orDefault(10000000);
	const int64_t versionsPerBatch = params.getInt("versionsPerBatch").orDefault(10000);
	const int64_t maxReadLag = params.getInt("maxReadLagVersions").orDefault(2000000);
	const int64_t maxReadKeys = params.getInt("maxReadKeys").orDefault(1000);
	const int64_t maxWriteKeys = params.getInt("maxWriteKeys").orDefault(10);
	const int64_t seed = params.getInt("seed").orDefault(1);
	const bool ranges = workload == "ranges";
	ASSERT(ranges || workload == "points");

	const Version window = SERVER_KNOBS->MAX_WRITE_TRANSACTION_LIFE_VERSIONS;
	const uint64_t residentBefore = getResidentMemoryUsage();
	ConflictSet* cs = newConflictSet();
	std::mt19937_64 random(seed);
	auto uniform = [&](int64_t lo, int64_t hi) { return lo + int64_t(random() % uint64_t(hi - lo + 1)); };

	Version now = window + versionsPerBatch;
	std::vector<double> latencies;
	int64_t transactions = 0, committed = 0, tooOld = 0;
	uint64_t decisionsHash = 1469598103934665603ULL;
	ConflictSetStats atStart;
	double addAtStart = 0, sortAtStart = 0;
	uint64_t residentAfterWarmup = 0;
	double measuredSeconds = 0;

	for (int b = 0; b < warmupBatches + batches; ++b) {
		if (b == warmupBatches) {
			atStart = getConflictSetStats(cs);
			addAtStart = evalAddSeconds;
			sortAtStart = g_sort.getValue();
			residentAfterWarmup = getResidentMemoryUsage();
		}
		Arena arena;
		std::vector<CommitTransactionRef> trs(transactionsPerBatch);
		for (CommitTransactionRef& tr : trs) {
			tr.read_snapshot = now - uniform(1, maxReadLag);
			for (int r = 0; r < readsPerTransaction; ++r) {
				const int64_t k = uniform(0, keySpace - 1);
				const StringRef from = evalBenchmarkKey(arena, k);
				const StringRef to = ranges ? evalBenchmarkKey(arena, k + uniform(1, maxReadKeys)) : keyAfter(from, arena);
				tr.read_conflict_ranges.push_back(arena, KeyRangeRef(from, to));
			}
			for (int w = 0; w < writesPerTransaction; ++w) {
				const int64_t k = uniform(0, keySpace - 1);
				const StringRef from = evalBenchmarkKey(arena, k);
				const StringRef to = ranges ? evalBenchmarkKey(arena, k + uniform(1, maxWriteKeys)) : keyAfter(from, arena);
				tr.write_conflict_ranges.push_back(arena, KeyRangeRef(from, to));
			}
		}

		std::vector<int> nonConflicting, tooOldList;
		std::map<int, VectorRef<int>> conflictingKeys;
		Arena replyArena;
		const bool measured = b >= warmupBatches;
		const double start = timer_monotonic();
		if (measured) {
			evalMeasuredBatch(cs, trs, now, now - window, nonConflicting, tooOldList, conflictingKeys, replyArena);
		} else {
			evalWarmupBatch(cs, trs, now, now - window, nonConflicting, tooOldList, conflictingKeys, replyArena);
		}
		const double elapsed = timer_monotonic() - start;
		if (measured) {
			latencies.push_back(elapsed);
			measuredSeconds += elapsed;
			transactions += trs.size();
			committed += nonConflicting.size();
			tooOld += tooOldList.size();
			std::vector<uint8_t> status(trs.size(), 1);
			for (int t : nonConflicting) {
				status[t] = 0;
			}
			for (int t : tooOldList) {
				status[t] = 2;
			}
			for (uint8_t s : status) {
				decisionsHash = (decisionsHash ^ s) * 1099511628211ULL;
			}
		}
		now += versionsPerBatch;
	}

	const ConflictSetStats atEnd = getConflictSetStats(cs);
	const uint64_t residentAfter = getResidentMemoryUsage();
	// The batch's phases, consecutive and without overlap: adding the transactions, then
	// detectConflicts' sort, read check, intra-batch check, combine, merge and cleanup. What the
	// batch's timer holds beyond them (the ConflictBatch itself, the decisions lists) is otherS.
	// seconds is the whole of every measured batch, and the figure that compares the indexes.
	const double addS = evalAddSeconds - addAtStart, sortS = g_sort.getValue() - sortAtStart;
	const double checkReadS = atEnd.checkReadSeconds - atStart.checkReadSeconds;
	const double intraBatchS = atEnd.checkIntraBatchSeconds - atStart.checkIntraBatchSeconds;
	const double combineS = atEnd.combineSeconds - atStart.combineSeconds;
	const double mergeS = atEnd.mergeSeconds - atStart.mergeSeconds;
	const double cleanupS = atEnd.cleanupSeconds - atStart.cleanupSeconds;
	printf("ConflictSetPhases index=%s workload=%s seed=%lld seconds=%.4f addS=%.4f sortS=%.4f checkReadS=%.4f "
	       "intraBatchS=%.4f combineS=%.4f mergeS=%.4f cleanupS=%.4f otherS=%.4f\n",
	       conflictSetIndexName(cs),
	       workload.c_str(),
	       (long long)seed,
	       measuredSeconds,
	       addS,
	       sortS,
	       checkReadS,
	       intraBatchS,
	       combineS,
	       mergeS,
	       cleanupS,
	       measuredSeconds - (addS + sortS + checkReadS + intraBatchS + combineS + mergeS + cleanupS));
	printf("ConflictSetBenchmark index=%s workload=%s seed=%lld batches=%d warmup=%d transactions=%lld "
	       "committed=%lld tooOld=%lld decisions=%016llx seconds=%.4f txPerSec=%.0f batchP50us=%.1f batchP90us=%.1f "
	       "batchP99us=%.1f batchMaxUs=%.1f checkReadS=%.4f intraBatchS=%.4f combineS=%.4f mergeS=%.4f cleanupS=%.4f "
	       "rssBeforeMiB=%.1f rssAfterWarmupMiB=%.1f rssAfterMiB=%.1f validationMinRV=%lld tooOldWithinWindow=%lld\n",
	       conflictSetIndexName(cs),
	       workload.c_str(),
	       (long long)seed,
	       batches,
	       warmupBatches,
	       (long long)transactions,
	       (long long)committed,
	       (long long)tooOld,
	       (unsigned long long)decisionsHash,
	       measuredSeconds,
	       double(transactions) / measuredSeconds,
	       evalPercentile(latencies, 0.5) * 1e6,
	       evalPercentile(latencies, 0.9) * 1e6,
	       evalPercentile(latencies, 0.99) * 1e6,
	       evalPercentile(latencies, 1.0) * 1e6,
	       atEnd.checkReadSeconds - atStart.checkReadSeconds,
	       atEnd.checkIntraBatchSeconds - atStart.checkIntraBatchSeconds,
	       atEnd.combineSeconds - atStart.combineSeconds,
	       atEnd.mergeSeconds - atStart.mergeSeconds,
	       atEnd.cleanupSeconds - atStart.cleanupSeconds,
	       residentBefore / 1048576.0,
	       residentAfterWarmup / 1048576.0,
	       residentAfter / 1048576.0,
	       (long long)atEnd.validationMinRV,
	       (long long)atEnd.tooOldWithinWindow);
	destroyConflictSet(cs);
	return Void();
}
