#include "tempconflictbtree.h"
#include "bnodestore.h"
#include <numeric>
#include <stdexcept>

#ifndef RESOLVER1_WRITE_LG
#define RESOLVER1_WRITE_LG 8
#endif
#ifndef RESOLVER1_WRITE_GROUPS
#define RESOLVER1_WRITE_GROUPS 2
#endif

TempConflictBTree::TempConflictBTree(std::size_t budget)
	: pool_(budget, BlockSize),
	  keyStore_(pool_),
	  nodeStore_(pool_, keyStore_) {
}

void TempConflictBTree::addWrites(
		const std::vector<KeyInfo>& points,
		TransactionInfo* const* transactionInfo,
		const bool* transactionConflictStatus,
		Version cv) {
	constexpr uint32_t LG = RESOLVER1_WRITE_LG;
	constexpr uint32_t TaskCapacity = LG == 0 ? 1 : LG;
	constexpr uint32_t GroupCount = RESOLVER1_WRITE_GROUPS;
	static_assert(LG <= 256, "write task groups must be bounded");
	static_assert(GroupCount == 2 || GroupCount == 3, "use two or three write groups");
	if constexpr (LG == 0) {
		addWritesSequential(points, transactionInfo, transactionConflictStatus, cv);
		return;
	}
	if (readWriteBegins.empty()) return;
	const auto endWrites = static_cast<uint32_t>(readWriteBegins.size());
	if (writeLeaves.empty()) {
		if (firstWriteBegin_ == endWrites) return;
		writeLeaves.push_back({nullptr, firstWriteBegin_, endWrites, nullptr});
	}
	if (!writeDestinationsActive_) prepareWriteDestinations();
	struct DestinationGuard {
		TempConflictBTree& tree;
		~DestinationGuard() { tree.clearWriteDestinations(); }
	} destinationGuard{*this};

	// Compact accepted unions into the existing begin records and links.
	// Decisions are final: transaction becomes endIndex only after filtering.
	// Keep firstWrite/nextOther intact for destination redirection. No key bytes
	// or per-write allocations are needed here; tasks consume the keys once.
	writePreparation_.assign(writeLeaves.size(), WritePreparation{});
	uint32_t pending = endWrites;
	for (uint32_t g = 0; g < writeLeaves.size(); ++g) {
		const auto& group = writeLeaves[g];
		auto& preparation = writePreparation_[g];
		preparation.firstAccepted = endWrites;
		for (uint32_t w = group.firstWrite; w < group.end();) {
			const uint32_t next = nextBegin[w];
			auto& entry = readWriteBegins[w];
			const auto& info = *transactionInfo[entry.transaction];
			if (!info.tooOld && !transactionConflictStatus[entry.transaction]) {
				const auto& point = points[entry.index()];
				const std::size_t r = (reinterpret_cast<const char*>(point.pIndex) -
					reinterpret_cast<const char*>(info.writeRanges.begin())) /
					sizeof(*info.writeRanges.begin());
				const uint32_t end = static_cast<uint32_t>(info.writeRanges[r].second);
				if (entry.index() < end) {
					if (pending != endWrites && entry.index() <= readWriteBegins[pending].transaction) {
						readWriteBegins[pending].transaction =
							std::max(readWriteBegins[pending].transaction, end);
					} else {
						if (pending != endWrites) nextBegin[pending] = w;
						pending = w;
						entry.transaction = end;
						nextBegin[w] = endWrites;
						if (preparation.firstAccepted == endWrites) preparation.firstAccepted = w;
					}
				}
			}
			w = next;
		}
	}

	enum class Step : uint8_t {
		Inspect, Route, InspectRight, CompareRight, Search, CompareSearch,
		SearchReady, Previous, InspectPrevious, Insert, Scan, CompareEnd, Erase,
		KeyMore, PrepareInput, Done
	};
	struct WriteTask {
		WriteCursor cursor;
		std::pair<StringRef, StringRef> write;
		LeafBlock* anchor = nullptr;
		LeafBlock* scan = nullptr;
		const KeyBlockHeader* keyBlock = nullptr;
		std::size_t keyOffset = 0, keyLeft = 0;
		std::size_t lo = 0, hi = 0, rank = 0, scanRank = 0;
		uint64_t mutation = 0;
		Version cutoff = 0;
		uint32_t group = 0, representative = 0, entry = 0;
		Step step = Step::Done, afterKey = Step::Done, afterPrepare = Step::Inspect;
		bool found = false, owned = false;
	};
	struct Group {
		std::array<WriteTask, TaskCapacity> tasks;
		uint32_t count = 0, first = 0;
		std::array<uint64_t, (TaskCapacity + 63) / 64> runnable{};
	};
	std::array<Group, GroupCount> groups;
	uint32_t nextGroup = 0;
	auto hintBytes = [](const void* address, std::size_t length) {
		if (length == 0) return;
		const auto* bytes = static_cast<const char*>(address);
		__builtin_prefetch(bytes);
		// Short keys touch at most two lines. Hint both endpoints without a
		// variable-trip loop; a duplicate hint to the same line is harmless.
		if (length <= 64) {
			__builtin_prefetch(bytes + length - 1);
			return;
		}
		const std::size_t first = 64 - (reinterpret_cast<std::uintptr_t>(bytes) & 63);
		for (std::size_t off = first; off < length; off += 64) __builtin_prefetch(bytes + off);
	};
	auto hintLeaf = [&](const LeafBlock* leaf) {
		if (leaf == nullptr) return false;
		hintBytes(leaf, BlockSize);
		return true;
	};
	auto setInput = [&](WriteTask& t) {
		const auto& entry = readWriteBegins[t.entry];
		t.write = {points[entry.index()].key, points[entry.transaction].key};
		hintBytes(t.write.first.begin(), t.write.first.size());
		hintBytes(t.write.second.begin(), t.write.second.size());
		return t.write.first.size() != 0 || t.write.second.size() != 0;
	};
	auto resetSearch = [&](WriteTask& t) {
		t.representative = resolveWriteGroup(t.group);
		const auto& destination = writeLeaves[t.representative];
		t.anchor = destination.leaf != nullptr ? destination.leaf : rightmostLeaf_;
		t.cursor = {t.anchor, destination.splitCandidate, writeLeavesSplitEpoch_,
			destination.leaf != nullptr && destination.needsRightCheck()};
		t.mutation = writePreparation_[t.representative].mutation;
		t.cutoff = reclaimCV_;
		t.lo = 0;
		t.found = false;
		t.step = Step::Inspect;
		hintLeaf(t.cursor.leaf);
	};
	auto fill = [&](Group& g) {
		g.count = g.first = 0;
		g.runnable.fill(0);
		while (g.count < TaskCapacity && nextGroup < writeLeaves.size()) {
			const uint32_t index = nextGroup++;
			if (writePreparation_[index].firstAccepted == endWrites) continue;
			g.runnable[g.count / 64] |= uint64_t{1} << (g.count % 64);
			auto& t = g.tasks[g.count++];
			t = WriteTask{};
			t.group = index;
			t.entry = writePreparation_[index].firstAccepted;
			setInput(t);
			resetSearch(t);
		}
	};
	// Hint one block of stored key bytes without following a cold link.
	// A linked-block header is hinted now and dereferenced on a later pass.
	auto hintKeyChunk = [&](WriteTask& t) {
		const std::size_t bytes = std::min(t.keyLeft, BlockSize - t.keyOffset);
		hintBytes(reinterpret_cast<const char*>(t.keyBlock) + t.keyOffset, bytes);
		t.keyLeft -= bytes;
		if (t.keyLeft != 0) {
			hintBytes(t.keyBlock, sizeof(KeyBlockHeader));
			t.step = Step::KeyMore;
		} else {
			t.step = t.afterKey;
		}
	};
	auto hintKey = [&](WriteTask& t, const RangeRef& range, bool wholeRange, Step after) {
		const std::size_t length = wholeRange ? std::max<std::size_t>(range.beginLength,
			std::size_t(range.endStart) + range.endLength) : range.beginLength;
		const std::size_t available = BlockSize - range.keyOffset;
		if (length <= available) {
			t.step = after;
			if (length == 0) return false;
			hintBytes(reinterpret_cast<const char*>(range.firstBlock) + range.keyOffset, length);
			return true;
		}
		// Persist continuation state only when this key spans linked blocks.
		t.keyBlock = range.firstBlock;
		t.keyOffset = range.keyOffset;
		t.keyLeft = length;
		t.afterKey = after;
		hintKeyChunk(t);
		return true;
	};
	auto finishInterval = [&](WriteTask& t) {
		t.entry = nextBegin[t.entry];
		if (t.entry >= writeLeaves[t.group].end()) {
			t.step = Step::Done;
			return false;
		}
		// rank identifies the insertion or its right exterior, both <= the
		// next accepted begin. Search only the remaining suffix of this leaf.
		t.lo = t.rank;
		t.found = false;
		t.afterPrepare = Step::Inspect;
		t.step = Step::PrepareInput;
		return setInput(t);
	};
	// A visit ends only after hinting its next dependency, parking, or finishing.
	enum class Visit : uint8_t { Prefetched, Parked, Done };
	auto prepareErase = [&](WriteTask& t) {
		t.step = Step::Erase;
		// eraseAt can hand the scan to a neighbour at the end of this leaf.
		// Otherwise the next entry is in the already warm leaf.
		if (t.scanRank + 1 < t.scan->header.count) return false;
		if (t.scan->right != nullptr) return hintLeaf(t.scan->right);
		return t.scan->header.count == 1 && hintLeaf(t.scan->left);
	};
	// Pure search: no reserve, cursor guard, split, scan or mutation arguments.
	// Returns true only after hinting the next dependency; false at SearchReady.
	auto advanceSearch = [&](WriteTask& t) __attribute__((always_inline)) -> bool {
		if (t.step == Step::KeyMore) {
			assert(t.afterKey == Step::CompareSearch);
			t.keyBlock = t.keyBlock->nextBlock;
			assert(t.keyBlock != nullptr);
			t.keyOffset = sizeof(KeyBlockHeader);
			hintKeyChunk(t);
			return true;
		}
		const auto* leaf = t.cursor.leaf;
		assert(leaf != nullptr);
		for (;;) {
			if (t.step == Step::CompareSearch) {
				const int compare = keyStore_.compareBegin(t.write.first, leaf->ranges[leaf->order[t.rank]]);
				if (compare == 0) {
					t.found = true;
					t.step = Step::SearchReady;
					return false;
				}
				if (compare < 0) t.hi = t.rank; else t.lo = t.rank + 1;
			}
			assert(t.lo <= t.hi);
			if (t.lo == t.hi) {
				t.rank = t.lo;
				t.found = false;
				t.step = Step::SearchReady;
				return false;
			}
			t.rank = t.lo + (t.hi - t.lo) / 2;
			if (hintKey(t, leaf->ranges[leaf->order[t.rank]], false, Step::CompareSearch)) return true;
			// Empty stored begins need no memory hint and no scheduler round trip.
		}
	};
	// Ownership, cleanup and mutations retain their own register allocation.
	auto advanceSlow = [&](WriteTask& t, bool head) __attribute__((noinline)) -> Visit {
		assert(t.step != Step::Done);
		assert(!t.owned || head);
		struct CursorGuard {
			TempConflictBTree& tree;
			bool head;
			~CursorGuard() { if (head) tree.activeWriteCursor_ = nullptr; }
		} guard{*this, head};
		assert(activeWriteCursor_ == nullptr);
		if (head) {
			if (!t.owned) {
				t.owned = true;
				t.afterPrepare = t.step;
				t.step = Step::PrepareInput;
			}
			activeWriteCursor_ = &t.cursor;
		}
		for (;;) {
			auto* leaf = t.cursor.leaf;
			switch (t.step) {
			case Step::PrepareInput: {
				// Enter once on ownership acquisition and once per next interval.
				// No reserve/input checks on search or scan continuations.
				assert(head && t.owned);
				if (t.write.first.compare(t.write.second) >= 0) {
					t.rank = t.lo;
					if (finishInterval(t)) return Visit::Prefetched;
					continue;
				}
				LeafBlock* before = t.cursor.leaf;
				const Version cutoff = reclaimCV_;
				reserveWrite(t.write);
				if (t.cursor.leaf == nullptr) t.cursor.leaf = rightmostLeaf_;
				t.step = t.afterPrepare;
				if (before != t.cursor.leaf || cutoff != reclaimCV_) {
					t.lo = 0;
					t.found = false;
					t.step = Step::Inspect;
					if (hintLeaf(t.cursor.leaf)) return Visit::Prefetched;
				}
				continue;
			}
			case Step::Inspect:
				if (leaf == nullptr) {
					t.rank = 0;
					t.found = false;
					t.step = Step::SearchReady;
					continue;
				}
				// Park before emitting hints: a blocked task is not polled again.
				if (!head && (cleanLeafCV_ != reclaimCV_ ||
					leaf->lastCleanReadBatch != readBatch_ || t.cursor.rightCheckRequired)) {
					return Visit::Parked;
				}
				if (head && cleanExpired(leaf)) t.lo = 0;
				if (t.cursor.rightCheckRequired) {
					t.step = Step::Route;
				} else {
					t.hi = leaf->header.count;
					t.step = Step::Search;
				}
				continue;
			case Step::Route:
				t.scan = leaf->right;
				t.step = Step::InspectRight;
				if (hintLeaf(t.scan)) return Visit::Prefetched;
				continue;
			case Step::InspectRight:
				if (t.scan == nullptr) {
					t.hi = leaf->header.count;
					t.step = Step::Search;
					continue;
				}
				t.scanRank = 0;
				while (t.scanRank < t.scan->header.count &&
					t.scan->cv[t.scan->order[t.scanRank]] <= reclaimCV_) ++t.scanRank;
				if (t.scanRank == t.scan->header.count) {
					t.scan = t.scan->right;
					if (hintLeaf(t.scan)) return Visit::Prefetched;
				} else if (hintKey(t, t.scan->ranges[t.scan->order[t.scanRank]], false, Step::CompareRight)) {
					return Visit::Prefetched;
				}
				continue;
			case Step::CompareRight:
				if (keyStore_.compareBegin(t.write.first, t.scan->ranges[t.scan->order[t.scanRank]]) < 0) {
					t.hi = leaf->header.count;
					t.step = Step::Search;
				} else {
					t.cursor.leaf = t.scan;
					if (leaf->header.count == 0) removeIfEmpty(&leaf->header);
					cleanExpired(t.cursor.leaf);
					t.cursor.splitEpoch = writeSplitEpoch_ - 1;
					t.lo = 0;
					t.step = Step::Route;
				}
				continue;
			case Step::Search:
			case Step::CompareSearch:
				if (advanceSearch(t)) return Visit::Prefetched;
				goto searchReady;
			case Step::SearchReady:
			searchReady:
				if (!head) return Visit::Parked;
				prepareWriteSplit(t.cursor, t.rank, t.found, cv);
				t.step = Step::Previous;
				goto previous;
			case Step::Previous:
			previous:
				if (t.cursor.leaf == nullptr || t.found) {
					t.step = Step::Insert;
				} else if (t.rank > 0) {
					t.scan = t.cursor.leaf;
					t.scanRank = t.rank - 1;
					const auto slot = t.scan->order[t.scanRank];
					t.step = Step::Insert;
					if (t.scan->cv[slot] > reclaimCV_ && hintKey(t, t.scan->ranges[slot], true, Step::Insert)) {
						return Visit::Prefetched;
					}
				} else {
					t.scan = t.cursor.leaf->left;
					t.step = Step::InspectPrevious;
					if (hintLeaf(t.scan)) return Visit::Prefetched;
				}
				continue;
			case Step::InspectPrevious:
				if (t.scan == nullptr) { t.step = Step::Insert; continue; }
				if (t.scan->header.count == 0) {
					t.scan = t.scan->left;
					if (hintLeaf(t.scan)) return Visit::Prefetched;
				} else {
					t.scanRank = t.scan->header.count - 1;
					const auto slot = t.scan->order[t.scanRank];
					t.step = Step::Insert;
					if (t.scan->cv[slot] > reclaimCV_ && hintKey(t, t.scan->ranges[slot], true, Step::Insert)) {
						return Visit::Prefetched;
					}
				}
				continue;
			case Step::Insert: {
				auto range = keyStore_.appendRange(t.write.first, t.write.second, cv);
				assert(range.has_value());
				if (t.cursor.leaf == nullptr) {
					t.cursor.leaf = createRootLeaf(cv);
					insertAt(t.rank, *range, cv, t.cursor.leaf);
					if (finishInterval(t)) return Visit::Prefetched;
				} else if (insertWithLeftExterior(t.write, *range, cv, t.found, t.cursor.leaf, t.rank)) {
					if (finishInterval(t)) return Visit::Prefetched;
				} else {
					t.scan = t.cursor.leaf;
					t.scanRank = t.rank + 1;
					t.step = Step::Scan;
				}
				continue;
			}
			case Step::Scan:
			scan:
				if (t.scan == nullptr) {
					if (finishInterval(t)) return Visit::Prefetched;
					continue;
				}
				if (t.scanRank >= t.scan->header.count) {
					t.scan = t.scan->right;
					t.scanRank = 0;
					if (hintLeaf(t.scan)) return Visit::Prefetched;
				} else {
					const auto slot = t.scan->order[t.scanRank];
					if (t.scan->cv[slot] > reclaimCV_) {
						if (hintKey(t, t.scan->ranges[slot], true, Step::CompareEnd)) return Visit::Prefetched;
					} else if (prepareErase(t)) return Visit::Prefetched;
				}
				continue;
			case Step::CompareEnd:
				if (keyStore_.compareEnd(t.write.second, t.scan->ranges[t.scan->order[t.scanRank]]) < 0) {
					preserveRightExterior(t.write.second, t.scan, t.scanRank);
					if (finishInterval(t)) return Visit::Prefetched;
				} else if (prepareErase(t)) return Visit::Prefetched;
				continue;
			case Step::Erase:
				eraseAt(t.scanRank, t.scan);
				// Its possible successor was hinted before eraseAt. The current
				// leaf needs no second full-block prefetch after deleting one entry.
				t.step = Step::Scan;
				goto scan;
			case Step::KeyMore:
				t.keyBlock = t.keyBlock->nextBlock;
				assert(t.keyBlock != nullptr);
				t.keyOffset = sizeof(KeyBlockHeader);
				hintKeyChunk(t);
				return Visit::Prefetched;
			case Step::Done: return Visit::Done;
			}
		}
	};

	// Validate before dereferencing speculative state, then take the small search
	// path when possible. Only an owned head can bypass PrepareInput.
	auto advance = [&](WriteTask& t, bool head) __attribute__((always_inline)) -> Visit {
		assert(t.step != Step::Done);
		assert(!t.owned || head);
		// Check metadata before consuming a speculative pointer. A cached root
		// stays valid until its parent changes; resolve only after a redirection.
		if (!t.owned) {
			const uint32_t representative =
				nextOther[writeLeaves[t.representative].firstWrite] == t.representative
				? t.representative : resolveWriteGroup(t.group);
			const auto& d = writeLeaves[representative];
			const LeafBlock* anchor = d.leaf != nullptr ? d.leaf : rightmostLeaf_;
			if (representative != t.representative || anchor != t.anchor ||
				writePreparation_[representative].mutation != t.mutation || reclaimCV_ != t.cutoff) {
				// Do not repeatedly redo a search while older writes keep changing
				// its destination. Park without dereferencing the saved pointers.
				if (!head) return Visit::Parked;
				resetSearch(t);
				if (t.cursor.leaf != nullptr) return Visit::Prefetched;
			}
		}
		if ((!head || t.owned) &&
			(t.step == Step::Search || t.step == Step::CompareSearch ||
			 (t.step == Step::KeyMore && t.afterKey == Step::CompareSearch))) {
			if (advanceSearch(t)) return Visit::Prefetched;
			if (!head) return Visit::Parked;
		}
		// A finished speculative search needs no mutation frame until ownership.
		if (!head && t.step == Step::SearchReady) return Visit::Parked;
		return advanceSlow(t, head);
	};

	for (auto& group : groups) fill(group);
	uint32_t oldest = 0;
	while (groups[oldest].count != 0) {
		auto& headGroup = groups[oldest];
		// This is the only wake-up of an otherwise parked task. Validation
		// occurs inside advance, before any saved leaf/key pointer is consumed.
		headGroup.runnable[headGroup.first / 64] |= uint64_t{1} << (headGroup.first % 64);
		for (unsigned which = 0; which < 2; ++which) {
			// The successor's leaves were hinted when its slot was filled.
			// Let half of the older group's tasks finish before consuming them.
			// Parked searches therefore cannot collapse the lead to one visit.
			if constexpr (GroupCount == 2) {
				if (which != 0 && headGroup.first < (headGroup.count + 1) / 2) break;
			}
			// With three slots only the oldest two execute. The third slot
			// is leaf-prefetched standby; rotation admits it after the oldest
			// group has completed, without polling or touching its leaves.
			const uint32_t slot = oldest + which < GroupCount
				? oldest + which : oldest + which - GroupCount;
			auto& group = groups[slot];
			for (std::size_t word = 0; word < group.runnable.size(); ++word) {
				uint64_t pendingTasks = group.runnable[word];
				while (pendingTasks != 0) {
					const unsigned bit = __builtin_ctzll(pendingTasks);
					const uint64_t mask = uint64_t{1} << bit;
					pendingTasks &= pendingTasks - 1;
					const uint32_t i = static_cast<uint32_t>(64 * word + bit);
					const Visit visit = advance(group.tasks[i], which == 0 && i == group.first);
					if (visit != Visit::Prefetched) group.runnable[word] &= ~mask;
					if (visit == Visit::Done) {
						assert(which == 0 && i == group.first);
						++group.first;
					}
				}
			}
		}
		if (groups[oldest].first == groups[oldest].count) {
			fill(groups[oldest]);
			oldest = oldest + 1 == GroupCount ? 0 : oldest + 1;
		}
	}
}

void TempConflictBTree::noteWriteLeafMutation(LeafBlock* leaf) {
	if (!writePreparation_.empty() && leaf->firstWrite != UINT32_MAX) {
		++writePreparation_[leaf->firstWrite].mutation;
	}
}

void TempConflictBTree::addWritesSequential(
		const std::vector<KeyInfo>& points,
		TransactionInfo* const* transactionInfo,
		const bool* transactionConflictStatus,
		Version cv) {
	if (readWriteBegins.empty()) return;
	if (writeLeaves.empty()) {
		const uint32_t firstWrite = firstWriteBegin_;
		if (firstWrite == readWriteBegins.size()) return;
		writeLeaves.push_back({nullptr, firstWrite,
			static_cast<uint32_t>(readWriteBegins.size()), nullptr});
	}
	if (!writeDestinationsActive_) prepareWriteDestinations();
	struct DestinationGuard {
		TempConflictBTree& tree;
		~DestinationGuard() { tree.clearWriteDestinations(); }
	} destinationGuard{*this};
	WriteCursor cursor;
	uint32_t cursorGroup = static_cast<uint32_t>(writeLeaves.size());
	uint32_t pendingGroup = 0;
	uint32_t pendingBegin = 0;
	uint32_t pendingEnd = 0;
	bool hasPending = false;
	auto applyPending = [&]() {
		const StringRef& begin = points[pendingBegin].key;
		const StringRef& end = points[pendingEnd].key;
		// Equal-key endpoint positions can describe an empty interval.
		if (begin.compare(end) >= 0) return;
		if (cursorGroup != pendingGroup) {
			const WriteLeaf& group = writeLeaves[resolveWriteGroup(pendingGroup)];
			if (group.leaf != nullptr) {
				cursor = {group.leaf, group.splitCandidate,
					writeLeavesSplitEpoch_, group.needsRightCheck()};
			} else {
				cursor = {rightmostLeaf_, nullptr, writeSplitEpoch_ - 1, false};
			}
			cursorGroup = pendingGroup;
		}
		addWrite(cursor, {begin, end}, cv);
	};
	constexpr uint32_t LG = RESOLVER1_WRITE_LG;
	static_assert(LG <= UINT32_MAX / 4, "write leaf group is too large");
	auto prefetchWriteLeaves = [&](uint32_t first) {
		if constexpr (LG != 0) {
			const std::size_t end = std::min(writeLeaves.size(), std::size_t(first) + LG);
			for (std::size_t g = first; g < end; ++g) {
				// Resolve now, without retaining pointers across subsequent mutations.
				const WriteLeaf& destination = writeLeaves[resolveWriteGroup(g)];
				const LeafBlock* leaf = destination.leaf != nullptr
					? destination.leaf : rightmostLeaf_;
				if (leaf == nullptr) continue;
				const auto* bytes = reinterpret_cast<const char*>(leaf);
				constexpr std::size_t LineBytes = 64;
				for (std::size_t offset = 0; offset < BlockSize; offset += LineBytes) {
					__builtin_prefetch(bytes + offset);
				}
			}
		}
	};
	// Prime batches A and B; when applying A, hint C, then D while applying B.
	prefetchWriteLeaves(0);
	prefetchWriteLeaves(LG);
	uint32_t nextPrefetchBoundary = 0;
	// Stream accepted begins in endpoint order while traversing prepared leaves.
	// Union uses indices only; key bytes are fetched when emitting an interval.
	// Adjacent equal-key endpoints may remain separate. The first begin owns
	// the prepared destination of the combined interval.
	for (uint32_t g = 0; g < writeLeaves.size(); ++g) {
		if constexpr (LG != 0) {
			if (g == nextPrefetchBoundary) {
				prefetchWriteLeaves(g + 2 * LG);
				nextPrefetchBoundary += LG;
			}
		}
		const WriteLeaf& group = writeLeaves[g];
		for (uint32_t w = group.firstWrite; w < group.end(); w = nextBegin[w]) {
			const RangeEntry& entry = readWriteBegins[w];
			assert(entry.isWrite());
			const TransactionInfo& info = *transactionInfo[entry.transaction];
			if (info.tooOld || transactionConflictStatus[entry.transaction]) continue;
			const KeyInfo& point = points[entry.index()];
			const std::size_t r =
				(reinterpret_cast<const char*>(point.pIndex) -
				 reinterpret_cast<const char*>(info.writeRanges.begin())) /
				sizeof(*info.writeRanges.begin());
			const auto& range = info.writeRanges[r];
			const uint32_t beginIndex = entry.index();
			const uint32_t endIndex = static_cast<uint32_t>(range.second);
			if (beginIndex >= endIndex) continue;
			if (hasPending && beginIndex > pendingEnd) {
				applyPending();
				hasPending = false;
			}
			if (!hasPending) {
				pendingBegin = beginIndex;
				pendingEnd = endIndex;
				pendingGroup = g;
				hasPending = true;
			} else if (endIndex > pendingEnd) {
				pendingEnd = endIndex;
			}
		}
	}
	if (hasPending) applyPending();
}
void TempConflictBTree::prepareWriteDestinations() {
	assert(!writeDestinationsActive_);
	if (writeLeaves.empty()) return;
	writeDestinationsActive_ = true;
	for (uint32_t g = 0; g < writeLeaves.size(); ++g) {
		WriteLeaf& group = writeLeaves[g];
		nextOther[group.firstWrite] = g;
		if (group.leaf == nullptr) continue;
		uint32_t& representative = group.leaf->firstWrite;
		if (representative == UINT32_MAX) {
			representative = g;
		} else {
			nextOther[group.firstWrite] = representative;
			if (group.needsRightCheck()) writeLeaves[representative].requireRightCheck();
		}
	}
}
uint32_t TempConflictBTree::resolveWriteGroup(uint32_t group) {
	assert(writeDestinationsActive_ && group < writeLeaves.size());
	uint32_t root = group;
	while (nextOther[writeLeaves[root].firstWrite] != root) {
		root = nextOther[writeLeaves[root].firstWrite];
	}
	while (group != root) {
		uint32_t& parent = nextOther[writeLeaves[group].firstWrite];
		const uint32_t next = parent;
		parent = root;
		group = next;
	}
	return root;
}
void TempConflictBTree::clearWriteDestinations() {
	if (!writeDestinationsActive_) return;
	for (uint32_t g = 0; g < writeLeaves.size(); ++g) {
		const WriteLeaf& group = writeLeaves[g];
		if (nextOther[group.firstWrite] == g && group.leaf != nullptr) {
			group.leaf->firstWrite = UINT32_MAX;
		}
	}
	writeDestinationsActive_ = false;
	writePreparation_.clear();
}
void TempConflictBTree::redirectWriteLeaf(LeafBlock* leaf, LeafBlock* replacement) {
	assert(leaf != replacement);
	if (activeWriteCursor_ != nullptr && activeWriteCursor_->leaf == leaf) {
		activeWriteCursor_->leaf = replacement;
		activeWriteCursor_->splitCandidate = nullptr;
		activeWriteCursor_->splitEpoch = writeSplitEpoch_ - 1;
		activeWriteCursor_->rightCheckRequired = true;
	}
	if (!writeDestinationsActive_ || leaf->firstWrite == UINT32_MAX) return;
	noteWriteLeafMutation(leaf);
	const uint32_t source = leaf->firstWrite;
	assert(nextOther[writeLeaves[source].firstWrite] == source);
	leaf->firstWrite = UINT32_MAX;
	WriteLeaf& group = writeLeaves[source];
	group.splitCandidate = nullptr;
	group.requireRightCheck();
	if (replacement != nullptr && replacement->firstWrite != UINT32_MAX) {
		noteWriteLeafMutation(replacement);
		writeLeaves[replacement->firstWrite].requireRightCheck();
		nextOther[group.firstWrite] = replacement->firstWrite;
		group.leaf = nullptr;
	} else {
		group.leaf = replacement;
		if (replacement != nullptr) replacement->firstWrite = source;
	}
}
void TempConflictBTree::redirectWriteSubtree(BNodeHeader* node, LeafBlock* replacement) {
	if (!writeDestinationsActive_ && activeWriteCursor_ == nullptr) return;
	LeafBlock* leaf = firstLeaf(node);
	LeafBlock* last = lastLeaf(node);
	for (;;) {
		redirectWriteLeaf(leaf, replacement);
		if (leaf == last) break;
		leaf = leaf->right;
	}
}
uint64_t TempConflictBTree::bufferWriteLeaf(const WriteLeaf& group) {
	const std::size_t count = writeLeafBufferNext_ - writeLeafBufferFirst_;
	std::size_t capacity = writeLeafBuffer_.capacity();
	assert(capacity > 0);
	if (count == capacity) {
		std::vector<BufferedWriteLeaf> grown;
		grown.reserve(2 * capacity);
		for (std::size_t i = 0; i < count; ++i) {
			grown.push_back(writeLeafBuffer_[(writeLeafBufferHead_ + i) % capacity]);
		}
		writeLeafBuffer_.swap(grown);
		writeLeafBufferHead_ = 0;
		capacity = writeLeafBuffer_.capacity();
	}
	const std::size_t position = (writeLeafBufferHead_ + count) % capacity;
	if (position == writeLeafBuffer_.size()) {
		writeLeafBuffer_.push_back({group, false});
	} else {
		assert(position < writeLeafBuffer_.size());
		writeLeafBuffer_[position] = {group, false};
	}
	return writeLeafBufferNext_++;
}
uint32_t TempConflictBTree::emitWriteLeaf(uint64_t ticket) {
	assert(ticket >= writeLeafBufferFirst_ && ticket < writeLeafBufferNext_);
	const std::size_t capacity = writeLeafBuffer_.capacity();
	const std::size_t position =
		(writeLeafBufferHead_ + ticket - writeLeafBufferFirst_) % capacity;
	auto& buffered = writeLeafBuffer_[position];
	assert(!buffered.released);
	const uint32_t opEnd = buffered.group.end();
	if (buffered.group.firstWrite < opEnd) {
		assert(writeLeaves.empty() || writeLeaves.back().firstWrite < buffered.group.firstWrite);
		writeLeaves.push_back(buffered.group);
	}
	buffered.released = true;
	// Prefix order can differ from arrival order. Reuse a ring position only
	// after its ticket and all earlier arrival tickets have been released.
	while (writeLeafBufferFirst_ < writeLeafBufferNext_ &&
		   writeLeafBuffer_[writeLeafBufferHead_].released) {
		++writeLeafBufferFirst_;
		writeLeafBufferHead_ = (writeLeafBufferHead_ + 1) % capacity;
	}
	return opEnd;
}
void TempConflictBTree::reserveWrite(const std::pair<StringRef, StringRef>& write) {
	// Reserve before using the cursor: reclamation can release its leaf.
	constexpr std::size_t MinWriteBlocks = 35;
	const std::size_t keyLength = std::max({maxKeyLength_,
		static_cast<std::size_t>(write.first.size()),
		static_cast<std::size_t>(write.second.size())});
	const std::size_t payload = keyStore_.blockSize() - sizeof(KeyBlockHeader);
	const std::size_t recordBlocks = (2 * keyLength + payload - 1) / payload;
	const std::size_t writeBlocks = 3 * recordBlocks + 2;
	if (!reclaimUntilAvail(std::max(MinWriteBlocks, writeBlocks)) &&
		pool_.availableBlocks() < writeBlocks) {
		throw std::length_error("TempConflictBTree: the pool cannot hold one write");
	}
	maxKeyLength_ = keyLength;
}

void TempConflictBTree::prepareWriteSplit(WriteCursor& cursor, std::size_t& rank, bool found, Version cv) {
	LeafBlock*& leaf = cursor.leaf;
	BNodeHeader* nodeNeedsSplit = nullptr;
	if (leaf != nullptr) {
		if (needsSplit(leaf)) {
			nodeNeedsSplit = &leaf->header;
		} else if (cursor.splitEpoch == writeSplitEpoch_) {
			nodeNeedsSplit = cursor.splitCandidate;
		} else {
			// Split-state changes invalidate saved candidates. Repair the cursor
			// once, then reuse it until the next split-state or topology change.
			for (InternalBlock* parent = leaf->parent(); parent != nullptr; parent = parent->parent()) {
				if (needsSplit(parent)) {
					nodeNeedsSplit = &parent->header;
					break;
				}
			}
		}
		cursor.splitCandidate = nodeNeedsSplit;
		cursor.splitEpoch = writeSplitEpoch_;
	}
	if (leaf == nullptr && root_ != nullptr) {
		throw std::logic_error("TempConflictBTree: missing prepared write destination");
	}
	if (nodeNeedsSplit != nullptr) {
		split(nodeNeedsSplit, leaf, rank, found, cv);
	}
}

void TempConflictBTree::addWrite(
		WriteCursor& cursor,
		const std::pair<StringRef, StringRef>& write,
		Version cv) {
	assert(activeWriteCursor_ == nullptr);
	activeWriteCursor_ = &cursor;
	struct CursorGuard {
		TempConflictBTree& tree;
		~CursorGuard() { tree.activeWriteCursor_ = nullptr; }
	} cursorGuard{*this};
	reserveWrite(write);
	LeafBlock*& leaf = cursor.leaf;
	if (leaf == nullptr) {
		leaf = rightmostLeaf_;
		cursor.splitCandidate = nullptr;
		cursor.splitEpoch = writeSplitEpoch_ - 1;
		cursor.rightCheckRequired = false; // A right-end anchor has no right boundary to cross.
	}
	std::size_t rank = 0;
	bool found = false;
	if (leaf != nullptr) {
		cleanExpired(leaf);
		// The original group is bounded by the descent's partition. Only a
		// split, redirection or ambiguous expired boundary requires this walk.
		if (cursor.rightCheckRequired) {
			for (LeafBlock* right = leaf->right; right != nullptr; right = right->right) {
				std::size_t first = 0;
				while (first < right->header.count &&
					   right->cv[right->order[first]] <= reclaimCV_) ++first;
				if (first == right->header.count) continue;
				if (keyStore_.compareBegin(write.first, right->ranges[right->order[first]]) < 0) break;
				LeafBlock* previous = leaf;
				leaf = right;
				if (previous->header.count == 0) removeIfEmpty(&previous->header);
				cleanExpired(leaf);
				cursor.splitEpoch = writeSplitEpoch_ - 1;
			}
		}
		found = nodeStore_.findKey(write.first, rank, leaf);
	}
	prepareWriteSplit(cursor, rank, found, cv);

	auto range = keyStore_.appendRange(write.first, write.second, cv);
	assert(range.has_value());
	if (leaf == nullptr) {
		leaf = createRootLeaf(cv);
		insertAt(rank, *range, cv, leaf);
	} else if (!insertWithLeftExterior(write, *range, cv, found, leaf, rank)) {
		LeafBlock* scanLeaf = leaf;
		std::size_t scanRank = rank;
		std::size_t slot = 0;
		if (nextRank(scanLeaf, scanRank, slot)) {
			eraseCoveredRanges(write.second, scanLeaf, scanRank);
			preserveRightExterior(write.second, scanLeaf, scanRank);
		}
	}
	// Splits retain the left anchor; later writes advance through leaf links.
}

bool TempConflictBTree::insertWithLeftExterior(
		const std::pair<StringRef, StringRef>& write,
		const RangeRef& range,
		Version cv,
		bool found,
		LeafBlock*& leaf,
		std::size_t& rank) {
	// Only the destination leaf has space reserved for insertion. A right
	// exterior must stay here even if its original range is in the left leaf.
	assert(leaf->freeSlotCount() >= 2);
	LeafBlock* previousLeaf = leaf;
	std::size_t previous = rank;
	std::size_t slot = 0;
	const bool hasPrevious = !found && prevRank(previousLeaf, previous, slot);
	// Expired entries can still name reclaimed key blocks.
	const bool expired = hasPrevious && previousLeaf->cv[slot] <= reclaimCV_;
	if (hasPrevious && !expired &&
			keyStore_.compareEnd(write.first, previousLeaf->ranges[slot]) < 0) {
		// The left exterior retains its original CV and physical position.
		const RangeRef oldRef = previousLeaf->ranges[slot];
		const Version oldCV = previousLeaf->cv[slot];
		auto left = keyStore_.appendRange(oldRef, write.first, oldCV);
		assert(left.has_value());
		noteWriteLeafMutation(previousLeaf);
		previousLeaf->ranges[slot] = *left;
		if (keyStore_.compareEnd(write.second, oldRef) < 0) {
			// Disjointness puts every later live entry beyond the old range.
			// Both the write and its right exterior fit in the prepared leaf.
			auto right = keyStore_.appendRange(write.second, oldRef, oldCV);
			assert(right.has_value());
			insertAt(rank, range, cv, leaf);
			++rank;
			insertAt(rank, *right, oldCV, leaf);
			return true;
		}
	}
	insertAt(rank, range, cv, leaf);
	if (expired) {
		// Insert first so deleting an expired predecessor cannot empty the
		// destination leaf. Disjointness excludes any earlier overlap.
		eraseAt(previous, previousLeaf);
		leaf = previousLeaf;
		rank = previous;
	}
	return false;
}

void TempConflictBTree::eraseCoveredRanges(
		const StringRef& end,
		LeafBlock*& leaf,
		std::size_t& rank) {
	// The preceding inserted write keeps the tree nonempty during erasure.
	assert(leaf != nullptr);
	while (rank < leaf->header.count) {
		const auto slot = leaf->order[rank];
		// Check the CV before reading potentially reclaimed key bytes.
		if (leaf->cv[slot] > reclaimCV_ &&
				keyStore_.compareEnd(end, leaf->ranges[slot]) < 0) {
			return;
		}
		eraseAt(rank, leaf);
	}
}

void TempConflictBTree::preserveRightExterior(
		const StringRef& end,
		LeafBlock* leaf,
		std::size_t rank) {
	if (leaf == nullptr || rank >= leaf->header.count) {
		return;
	}
	const auto slot = leaf->order[rank];
	const RangeRef& oldRef = leaf->ranges[slot];
	if (keyStore_.compareBegin(end, oldRef) <= 0 ||
			keyStore_.compareEnd(end, oldRef) >= 0) {
		return;
	}
	auto right = keyStore_.appendRange(end, oldRef, leaf->cv[slot]);
	assert(right.has_value());
	noteWriteLeafMutation(leaf);
	leaf->ranges[slot] = *right;
	if (rank == 0) {
		// The first key of the leaf has changed; its CV has not.
		propagateUpdates(leaf, 0, true, false);
	}
}

LeafBlock* TempConflictBTree::createRootLeaf(Version cv) {
	assert(root_ == nullptr);
	LeafBlock* leaf = nodeStore_.allocateLeaf(cv);
	assert(leaf != nullptr);
	root_ = &leaf->header;
	rightmostLeaf_ = leaf;
	return leaf;
}

bool TempConflictBTree::findKey(
		const StringRef& key,
		LeafBlock*& leaf,
		std::size_t& rank,
	    BNodeHeader*& nodeNeedsSplit) {

	leaf = nullptr;
	rank = 0;
	nodeNeedsSplit = nullptr;
	if (root_ == nullptr) {
		return false;
	}
	std::vector<InternalBlock*> needSplit;
	BNodeHeader* current = root_;
	while (current->kind == BNodeKind::Internal) {
		InternalBlock* internal = reinterpret_cast<InternalBlock*>(current);
		refreshSeparators(internal);
		if (internal->header.count == 0) {
			assert(current == root_);
			root_ = nullptr;
			rightmostLeaf_ = nullptr;
			++writeSplitEpoch_;
			nodeStore_.releaseNode(current);
			return false;
		}
		if (needsSplit(internal)) {
			needSplit.push_back(internal);
		}
		const bool found = nodeStore_.findKey(key, rank, internal);
		if (!found && rank > 0) {
			--rank;
		}
		current = internal->children[internal->order[rank]];
	}

	leaf = reinterpret_cast<LeafBlock*>(current);
	cleanExpired(leaf);
	nodeNeedsSplit = findSplitCandidate(leaf, needSplit);
	return nodeStore_.findKey(key, rank, leaf);
}

BNodeHeader* TempConflictBTree::findSplitCandidate(
		LeafBlock* leaf,
		std::vector<InternalBlock*>& candidates) {
	if (needsSplit(leaf)) {
		return &leaf->header;
	}
	// Cleanup can change split state after a candidate was recorded.
	while (!candidates.empty()) {
		InternalBlock* node = candidates.back();
		candidates.pop_back();
		if (needsSplit(node)) {
			return &node->header;
		}
	}
	return nullptr;
}


void  TempConflictBTree::insertAt(
		std::size_t& rank,
		const RangeRef& range,
		Version cv,
		LeafBlock*& leaf) {
	const bool splitBefore = needsSplit(leaf);
	noteWriteLeafMutation(leaf);
	nodeStore_.insertAt(rank, range, cv, leaf);
	if (cv <= reclaimCV_) leaf->lastCleanReadBatch = readBatch_ - 1;
	const bool splitAfter = needsSplit(leaf);
	propagateUpdates(leaf, !splitBefore && splitAfter ? 1 : 0, rank == 0, true);
}

void TempConflictBTree::normalizeRoot() {
	while (root_ != nullptr) {
		if (root_->count == 0) {
			if (root_->kind == BNodeKind::Leaf) {
				redirectWriteLeaf(reinterpret_cast<LeafBlock*>(root_), nullptr);
			}
			++writeSplitEpoch_;
			nodeStore_.releaseNode(root_);
			root_ = nullptr;
			rightmostLeaf_ = nullptr;
			return;
		}
		if (root_->kind != BNodeKind::Internal || root_->count != 1) {
			return;
		}
		auto* oldRoot = reinterpret_cast<InternalBlock*>(root_);
		root_ = oldRoot->children[oldRoot->order[0]];
		root_->parent = nullptr;
		root_->parentSlot = 0;
		++writeSplitEpoch_;
		nodeStore_.releaseNode(oldRoot);
	}
}

void  TempConflictBTree::eraseAt(std::size_t& rank, LeafBlock*& leaf) {
	bool splitBefore=needsSplit(leaf);
	noteWriteLeafMutation(leaf);
	nodeStore_.eraseAt(rank,leaf);
	propagateUpdates(leaf, splitBefore && !needsSplit(leaf) ? -1 : 0,
					 rank == 0 && leaf->header.count > 0, false);
	if (leaf->header.count==0) {
		if (leaf->parent()==nullptr){
			redirectWriteLeaf(leaf, nullptr);
			rightmostLeaf_ = nullptr;
			++writeSplitEpoch_;
			nodeStore_.releaseNode(leaf);
			root_=nullptr;
			leaf=nullptr;
		} else {
			LeafBlock* oldLeft=leaf->left;
			LeafBlock* oldRight=leaf->right;
			InternalBlock* oldParent=leaf->parent();
			removeEmptyChild(leaf->parent(), leaf->parentRank());
			// Empty ancestors have no separator to propagate; remove them
			// before updating the first surviving ancestor.
			while (oldParent->header.count == 0 && oldParent->parent() != nullptr) {
				InternalBlock* tmp = oldParent;
				oldParent = tmp->parent();
				removeEmptyChild(oldParent, tmp->parentRank());
			}
			if (oldParent->header.count > 0) {
				propagateUpdates(oldParent, 0, true, false);
			}
			normalizeRoot();
			if (oldRight!=nullptr) {
				leaf= oldRight;
				rank= 0;
			} else if (oldLeft!=nullptr) {
				leaf= oldLeft;
				rank=leaf->header.count;
			} else {
				assert(root_ == nullptr);
				leaf=nullptr;
				rank=0;
			}
		}
	} else {
		if (rank==leaf->header.count){
			if (leaf->right!=nullptr) {
				leaf= leaf->right;
				rank= 0;
			}
		}
	}
}

BNodeHeader* TempConflictBTree::eraseAt(std::size_t rank, InternalBlock* node) {
	BNodeHeader* child = node->children[node->order[rank]];
	// The subtree's leaves are a contiguous run of the leaf chain.
	LeafBlock* first = firstLeaf(child);
	LeafBlock* last = lastLeaf(child);
	redirectWriteSubtree(child, last->right);
	if (last->right == nullptr) rightmostLeaf_ = first->left;
	unlinkLeaves(first, last);
	const bool splitBefore = needsSplit(node);
	if (needsSplit(child)) {
		--node->header.childrenNeedingSplit;
	}
	nodeStore_.eraseAt(rank, node);
	child->parent = nullptr;
	child->parentSlot = 0;
	// Removing a child frees a slot and cannot add a pending split.
	assert(!needsSplit(node));
	propagateUpdates(node, splitBefore ? -1 : 0, false, false);
	// Empty ancestors have no first key to propagate.
	while (node->header.count == 0 && node->parent() != nullptr) {
		InternalBlock* tmp = node;
		node = tmp->parent();
		removeEmptyChild(node, tmp->parentRank());
	}
	if (node->header.count > 0) {
		propagateUpdates(node, 0, true, false);
	}
	normalizeRoot();
	return child;
}

void TempConflictBTree::unlinkLeaves(LeafBlock* first, LeafBlock* last) {
	if (first->left != nullptr) {
		first->left->right = last->right;
	}
	if (last->right != nullptr) {
		last->right->left = first->left;
	}
	first->left = nullptr;
	last->right = nullptr;
}

LeafBlock* TempConflictBTree::firstLeaf(BNodeHeader* node) {
	while (node->kind == BNodeKind::Internal) {
		auto* internal = reinterpret_cast<InternalBlock*>(node);
		node = internal->children[internal->order[0]];
	}
	return reinterpret_cast<LeafBlock*>(node);
}

LeafBlock* TempConflictBTree::lastLeaf(BNodeHeader* node) {
	while (node->kind == BNodeKind::Internal) {
		auto* internal = reinterpret_cast<InternalBlock*>(node);
		node = internal->children[internal->order[internal->header.count - 1]];
	}
	return reinterpret_cast<LeafBlock*>(node);
}

void  TempConflictBTree::removeSubtreeRec(BNodeHeader* node) {

	if (node->kind==BNodeKind::Internal) {
		auto* internal = reinterpret_cast<InternalBlock*>(node);
		for (size_t rank=0;rank<node->count;rank++){
			size_t order=internal->order[rank];
			removeSubtreeRec(internal->children[order]);
		}
	}
	++writeSplitEpoch_;
	nodeStore_.releaseNode(node);
}

void  TempConflictBTree::removeSubtree(BNodeHeader* node) {
	if (node->parent==nullptr){
		redirectWriteSubtree(node, nullptr);
		root_=nullptr;
		rightmostLeaf_ = nullptr;
	} else {
		eraseAt(node->parentRank(), node->parent);
	}
	removeSubtreeRec(node);	
}

bool TempConflictBTree::reclaimOldest() {
	const std::optional<Version> keyCV = keyStore_.oldestCV();
	const std::optional<Version> nodeCV = nodeStore_.oldestCV();
	if (!keyCV && !nodeCV) {
		return false;
	}
	const Version minCV = !keyCV ? *nodeCV
		: !nodeCV ? *keyCV : std::min(*keyCV, *nodeCV);
	// Exteriors can append older records to later key blocks. Allocation
	// order does not imply increasing CVs, so the cutoff must not decrease.
	reclaimCV_ = std::max(reclaimCV_, minCV);
	while (nodeStore_.oldestCV() == minCV) {
		removeSubtree(nodeStore_.oldestNode());
	}
	while (keyStore_.oldestCV() == minCV) {
		keyStore_.reclaimOldest();
	}
	return true;
}

bool TempConflictBTree::reclaimUntilAvail(std::size_t n) {
	while (pool_.availableBlocks() < n) {
		if (!reclaimOldest()) {
			return false;
		}
	}
	return true;
}

bool TempConflictBTree::refreshSeparator(
		InternalBlock* node, std::size_t rank) {
	const auto slot = node->order[rank];
	if (node->separatorsCV[slot] > reclaimCV_) {
		return false;
	}
	if (node->children[slot]->kind == BNodeKind::Internal) {
		auto* child =
			reinterpret_cast<InternalBlock*>(node->children[slot]);
		// Removing the first grandchild exposes the next one, whose
		// separator may also have expired.
		while (child->header.count > 0 &&
			   child->separatorsCV[child->order[0]] <= reclaimCV_) {
			refreshSeparator(child, 0);
		}
		if (child->header.count == 0) {
			removeEmptyChild(node, rank);
		} else {
			const auto slot0 = child->order[0];
			node->separatorsCV[slot] = child->separatorsCV[slot0];
			node->separators[slot] = child->separators[slot0];
		}
	} else {
		auto* child =
			reinterpret_cast<LeafBlock*>(node->children[slot]);
		cleanExpired(child);
		if (child->header.count == 0) {
			removeEmptyChild(node, rank);
		} else {
			const auto slot0 = child->order[0];
			node->separatorsCV[slot] = child->cv[slot0];
			node->separators[slot] = child->ranges[slot0];
		}
	}
	return true;
}

bool TempConflictBTree::refreshSeparators(InternalBlock* node) {
	bool changed = false;
	std::size_t rank = 0;

	while (rank < node->header.count) {
		const auto count = node->header.count;
		changed |= refreshSeparator(node, rank);
		if (node->header.count == count) {
			++rank;
		}
	}
	return changed;
}

bool TempConflictBTree::refreshSeparatorDeferred(
		InternalBlock* node, std::size_t rank) {
	if (refreshLiveSeparator(node, rank)) {
		return true;
	}
	for (std::size_t r = rank; r-- > 0;) {
		if (refreshLiveSeparator(node, r)) {
			markEmptyChildren(node, r + 1, rank + 1, r);
			return false;
		}
	}
	for (std::size_t r = rank + 1; r < node->header.count; ++r) {
		if (refreshLiveSeparator(node, r)) {
			markEmptyChildren(node, 0, r, r);
			return false;
		}
	}
	// Every child was checked and has no live entry. There is no live
	// neighbour to copy; leave the key references unread and mark them all.
	// Read tasks still descend left and scan forward: a read may extend
	// beyond this empty subtree into live writes in a later subtree.
	for (std::size_t r = 0; r < node->header.count; ++r) {
		node->separatorsCV[node->order[r]] = EmptyChildCV;
	}
	return false;
}

std::optional<std::pair<const RangeRef*, Version>>
TempConflictBTree::firstLiveKey(BNodeHeader* node) {
	if (node->kind == BNodeKind::Leaf) {
		auto* leaf = reinterpret_cast<LeafBlock*>(node);
		for (std::size_t rank = 0; rank < leaf->header.count; ++rank) {
			const auto slot = leaf->order[rank];
			if (leaf->cv[slot] > reclaimCV_) {
				if (rank > 0) {
					pending_.emplace_back(node, node->generation);
				}
				return std::pair<const RangeRef*, Version>{
					&leaf->ranges[slot], leaf->cv[slot]};
			}
		}
		if (leaf->header.count > 0) {
			pending_.emplace_back(node, node->generation);
		}
		return std::nullopt;
	}
	auto* internal = reinterpret_cast<InternalBlock*>(node);
	for (std::size_t rank = 0; rank < internal->header.count; ++rank) {
		if (refreshLiveSeparator(internal, rank)) {
			markEmptyChildren(internal, 0, rank, rank);
			const auto slot = internal->order[rank];
			return std::pair<const RangeRef*, Version>{
				&internal->separators[slot], internal->separatorsCV[slot]};
		}
	}
	return std::nullopt;
}

bool TempConflictBTree::refreshLiveSeparator(
		InternalBlock* node, std::size_t rank) {
	const auto slot = node->order[rank];
	const Version cv = node->separatorsCV[slot];
	// Without writes or a changing cutoff, an empty marker stays valid
	// throughout deferred cleanup.
	if (cv == EmptyChildCV) {
		return false;
	}
	if (cv > reclaimCV_) {
		return true;
	}
	const auto first = firstLiveKey(node->children[slot]);
	if (!first) {
		return false;
	}
	node->separators[slot] = *first->first;
	node->separatorsCV[slot] = first->second;
	return true;
}

void TempConflictBTree::markEmptyChildren(InternalBlock* node,
		std::size_t first, std::size_t last, std::size_t from) {
	const auto source = node->order[from];
	for (std::size_t rank = first; rank < last; ++rank) {
		const auto slot = node->order[rank];
		node->separators[slot] = node->separators[source];
		node->separatorsCV[slot] = EmptyChildCV;
	}
}

bool TempConflictBTree::cleanExpired(LeafBlock* leaf) {
	// Writes can advance the cutoff after read cursors have been retired.
	if (cleanLeafCV_ != reclaimCV_) {
		cleanLeafCV_ = reclaimCV_;
		++readBatch_;
	}
	if (leaf->lastCleanReadBatch == readBatch_) return false;
	const bool splitBefore = needsSplit(leaf);
	bool deleted = false;
	std::size_t rank = 0;

	while (rank < leaf->header.count) {
		const auto slot = leaf->order[rank];
		if (leaf->cv[slot] <= reclaimCV_) {
			noteWriteLeafMutation(leaf);
			nodeStore_.eraseAt(rank, leaf);
			deleted = true;
		} else {
			++rank;
		}
	}

	if (splitBefore && !needsSplit(leaf)) {
		propagateUpdates(leaf, -1, false, false);
	}
	leaf->lastCleanReadBatch = readBatch_;
	return deleted;
}

void TempConflictBTree::removeEmptyChild(
		InternalBlock* parent, std::size_t rank) {
	const auto slot = parent->order[rank];
	BNodeHeader* child = parent->children[slot];
	assert(child->count == 0);

	const bool splitBefore = needsSplit(parent);

	if (child->kind == BNodeKind::Leaf) {
		auto* leaf = reinterpret_cast<LeafBlock*>(child);
		redirectWriteLeaf(leaf, leaf->right);
		if (leaf->right == nullptr) rightmostLeaf_ = leaf->left;
		unlinkLeaf(leaf);
	}

	// The empty child's split-state transition has already been propagated.
	nodeStore_.eraseAt(rank, parent);
	++writeSplitEpoch_;
	nodeStore_.releaseNode(child);

	if (splitBefore && !needsSplit(parent)) {
		propagateUpdates(parent, -1, false, false);
	}
}

void TempConflictBTree::removeIfEmpty(BNodeHeader* node) {
	if (node->count > 0) {
		return;
	}
	InternalBlock* parent = node->parent;
	if (parent == nullptr) {
		if (node->kind == BNodeKind::Internal) {
			root_ = nullptr;
			rightmostLeaf_ = nullptr;
			++writeSplitEpoch_;
			nodeStore_.releaseNode(node);
		}
		return;
	}
	removeEmptyChild(parent, node->parentRank());
	while (parent->header.count == 0 && parent->parent() != nullptr) {
		InternalBlock* tmp = parent;
		parent = tmp->parent();
		removeEmptyChild(parent, tmp->parentRank());
	}
	if (parent->header.count > 0) {
		propagateUpdates(parent, 0, true, false);
	}
	normalizeRoot();
}

void TempConflictBTree::split(
		BNodeHeader* node,
		LeafBlock*& leaf,
		std::size_t& rank,
		bool found,
		Version cv) {
	assert(node != nullptr);
	assert(needsSplit(node));
	++writeSplitEpoch_;
	assert(cv >= node->maxCV);

	BNodeHeader* sibling;
	if (node->kind == BNodeKind::Internal) {
		sibling = &splitInternal(reinterpret_cast<InternalBlock*>(node), cv)->header;
	} else {
		assert(leaf == reinterpret_cast<LeafBlock*>(node));
		sibling = &splitLeaf(leaf, rank, found, cv)->header;
	}
	insertSibling(node, sibling, cv);
	propagateUpdates(sibling, 0, false, true);
}

InternalBlock* TempConflictBTree::splitInternal(InternalBlock* oldNode, Version cv) {
	// Use the write CV as a conservative bound so allocation
	// can append the node to the age list.
	InternalBlock* newNode = nodeStore_.allocateInternal(cv);
	assert(newNode != nullptr);

	// Cut where the prefix reaches half the weight, counting twice the
	// children that need a split.
	int remainingWeight =
		(oldNode->header.count + oldNode->header.childrenNeedingSplit) / 2;
	int cutRank = 0;
	while (remainingWeight > 0) {
		const auto slot = oldNode->order[cutRank];
		remainingWeight -= needsSplit(oldNode->children[slot]) ? 2 : 1;
		++cutRank;
	}
	assert(cutRank > 0);
	assert(cutRank < oldNode->header.count);

	const int oldCount = oldNode->header.count;
	for (int currentRank = cutRank; currentRank < oldCount; ++currentRank) {
		const auto oldSlot = oldNode->order[currentRank];
		const auto newSlot = newNode->header.count;
		BNodeHeader* child = oldNode->children[oldSlot];
		newNode->order[newSlot] = newSlot;
		newNode->separators[newSlot] = oldNode->separators[oldSlot];
		newNode->separatorsCV[newSlot] = oldNode->separatorsCV[oldSlot];
		newNode->children[newSlot] = child;
		newNode->maxSubtreesCV[newSlot] = oldNode->maxSubtreesCV[oldSlot];
		newNode->unsetFree(newSlot);
		child->parent = newNode;
		child->parentSlot = newSlot;
		if (needsSplit(child)) {
			++newNode->header.childrenNeedingSplit;
			--oldNode->header.childrenNeedingSplit;
		}
		oldNode->setFree(oldSlot);
		++newNode->header.count;
	}
	oldNode->header.count = static_cast<std::uint8_t>(cutRank);
	return newNode;
}

LeafBlock* TempConflictBTree::splitLeaf(
		LeafBlock*& leaf,
		std::size_t& rank,
		bool found,
		Version cv) {
	LeafBlock* oldNode = leaf;
	noteWriteLeafMutation(oldNode);
	// A local split can place later begins from this prepared group on the
	// new right half. Unrelated leaves and internal splits do not invalidate it.
	if (activeWriteCursor_ != nullptr && activeWriteCursor_->leaf == oldNode) {
		activeWriteCursor_->rightCheckRequired = true;
	}
	if (writeDestinationsActive_ && oldNode->firstWrite != UINT32_MAX) {
		writeLeaves[oldNode->firstWrite].requireRightCheck();
	}
	LeafBlock* newNode = nodeStore_.allocateLeaf(cv);
	assert(newNode != nullptr);

	const std::size_t cutRank = oldNode->header.count / 2;
	const std::size_t oldCount = oldNode->header.count;
	assert(cutRank > 0);
	assert(cutRank < oldCount);
	for (std::size_t currentRank = cutRank; currentRank < oldCount; ++currentRank) {
		const auto oldSlot = oldNode->order[currentRank];
		const auto newSlot = newNode->header.count;
		newNode->order[newSlot] = newSlot;
		newNode->ranges[newSlot] = oldNode->ranges[oldSlot];
		newNode->cv[newSlot] = oldNode->cv[oldSlot];
		newNode->unsetFree(newSlot);
		oldNode->setFree(oldSlot);
		++newNode->header.count;
	}
	oldNode->header.count = static_cast<std::uint8_t>(cutRank);

	newNode->lastCleanReadBatch = oldNode->lastCleanReadBatch;
	newNode->left = oldNode;
	newNode->right = oldNode->right;
	if (newNode->right != nullptr) {
		newNode->right->left = newNode;
	}
	oldNode->right = newNode;
	if (newNode->right == nullptr) rightmostLeaf_ = newNode;

	if (rank > cutRank || (found && rank == cutRank)) {
		leaf = newNode;
		rank -= cutRank;
	}
	return newNode;
}

std::pair<const RangeRef*, Version> TempConflictBTree::minSeparator(
		const BNodeHeader* node) const {
	assert(node->count > 0);
	if (node->kind == BNodeKind::Leaf) {
		const auto* leaf = reinterpret_cast<const LeafBlock*>(node);
		const auto first = leaf->order[0];
		return {&leaf->ranges[first], leaf->cv[first]};
	}
	const auto* internal = reinterpret_cast<const InternalBlock*>(node);
	const auto first = internal->order[0];
	return {&internal->separators[first], internal->separatorsCV[first]};
}

void TempConflictBTree::insertSibling(
		BNodeHeader* node,
		BNodeHeader* sibling,
		Version cv) {
	// Both halves of a split stop needing one.
	assert(!needsSplit(node));
	assert(!needsSplit(sibling));

	InternalBlock* parent = node->parent;
	if (parent == nullptr) {
		parent = nodeStore_.allocateInternal(cv);
		assert(parent != nullptr);
		const auto [oldKey, oldCV] = minSeparator(node);
		const auto [newKey, newCV] = minSeparator(sibling);
		nodeStore_.insertAt(0, *oldKey, node, oldCV, parent);
		nodeStore_.insertAt(1, *newKey, sibling, newCV, parent);
		root_ = &parent->header;
		return;
	}

	// Consuming one reserved slot removes one pending-child contribution.
	// Free slots minus pending children, and hence split state, is unchanged.
	const bool parentSplitBefore = needsSplit(parent);
	assert(parent->freeSlotCount() > 0);
	assert(parent->header.childrenNeedingSplit > 0);
	const auto [key, keyCV] = minSeparator(sibling);
	nodeStore_.insertAt(node->parentRank() + 1, *key, sibling, keyCV, parent);
	--parent->header.childrenNeedingSplit;
	assert(needsSplit(parent) == parentSplitBefore);
	(void)parentSplitBefore;
}

void TempConflictBTree::propagateUpdates(
		BNodeHeader* node,
		int splitDelta,
		bool updateSeparator,
		bool updateCV) {
	assert(splitDelta >= -1 && splitDelta <= 1);
	if (splitDelta != 0) ++writeSplitEpoch_;

	// The first range and its CV propagate unchanged through ancestors
	// whose first child is on this path; read the descriptor only once.
	const RangeRef* separator = nullptr;
	Version separatorCV = 0;
	if (updateSeparator) {
		std::tie(separator, separatorCV) = minSeparator(node);
	}

	for (InternalBlock* parent = node->parent;
		 parent != nullptr && (splitDelta != 0 || updateSeparator || updateCV);
		 node = &parent->header, parent = node->parent) {
		const auto slot = node->parentSlot;

		if (splitDelta != 0) {
			const bool before = needsSplit(parent);
			assert(splitDelta > 0 || parent->header.childrenNeedingSplit > 0);
			parent->header.childrenNeedingSplit =
				static_cast<std::uint8_t>(parent->header.childrenNeedingSplit + splitDelta);
			if (before == needsSplit(parent)) {
				splitDelta = 0;
			}
		}

		if (updateSeparator) {
			parent->separators[slot] = *separator;
			parent->separatorsCV[slot] = separatorCV;
			updateSeparator = parent->order[0] == slot;
		}

		if (updateCV) {
			if (parent->maxSubtreesCV[slot] < node->maxCV) {
				parent->maxSubtreesCV[slot] = node->maxCV;
			}
			updateCV = parent->header.maxCV < node->maxCV;
			if (updateCV) {
				nodeStore_.updateCV(parent, node->maxCV);
			}
		}
	}
}

bool TempConflictBTree::hasConflict(const ReadConflictRange& read) {
    LeafBlock* leaf = nullptr;
    std::size_t rank = 0;
    BNodeHeader* nodeNeedsSplit = nullptr;
    const bool found = findKey(read.begin, leaf, rank, nodeNeedsSplit);
    if (leaf == nullptr) {
        return false;
    }
    std::size_t slot = 0;
    bool slotPresent = false;
    if (found) {
        slot = leaf->order[rank];
        slotPresent = true;
    } else {
        slotPresent = prevRank(leaf, rank, slot);
        // Check the CV before accessing potentially reclaimed key bytes.
        if (slotPresent && leaf->cv[slot] > read.version &&
            keyStore_.compareEnd(read.begin, leaf->ranges[slot]) < 0) {
            return true;
        }
        if (slotPresent) {
            slotPresent = nextRank(leaf, rank, slot);
        } else if (rank < leaf->header.count) {
            slot = leaf->order[rank];
            slotPresent = true;
        } else {
            slotPresent = nextRank(leaf, rank, slot);
        }
    }
    while (slotPresent) {
        // Skip expired entries without accessing their key bytes.
        if (leaf->cv[slot] > reclaimCV_) {
            if (keyStore_.compareBegin(read.end, leaf->ranges[slot]) <= 0) {
                break;
            }
            if (leaf->cv[slot] > read.version) {
                return true;
            }
        }
        slotPresent = nextRank(leaf, rank, slot);
    }
    return false;
}

bool TempConflictBTree::nextRank(
        LeafBlock*& leaf,
        std::size_t& rank,
        std::size_t& slot) const {
    if (rank + 1 < leaf->header.count) {
        ++rank;
        slot = leaf->order[rank];
        return true;
    }
    LeafBlock* next = leaf->right;
    if (next == nullptr) {
        rank = leaf->header.count;
        return false;
    }
    // Only the root leaf may be empty, and it has no neighbours.
    leaf = next;
    rank = 0;
    slot = leaf->order[rank];
    return true;
}

bool TempConflictBTree::prevRank(
        LeafBlock*& leaf,
        std::size_t& rank,
        std::size_t& slot) const {
    if (rank > 0) {
        --rank;
        slot = leaf->order[rank];
        return true;
    }
    LeafBlock* previous = leaf->left;
    if (previous == nullptr) {
        return false;
    }
    leaf = previous;
    rank = leaf->header.count - 1;
    slot = leaf->order[rank];
    return true;
}

void TempConflictBTree::cleanPending() {
	for (const auto& [node, generation] : pending_) {
		// Duplicate records are allowed. Release clears the generation, and
		// no allocation can reuse that block before the queue is drained.
		if (node->generation != generation) {
			continue;
		}
		cleanExpired(reinterpret_cast<LeafBlock*>(node));
		removeIfEmpty(node);
	}
	pending_.clear();
}

template<uint8_t G>
void TempConflictBTree::checkReads(
			const std::vector<KeyInfo>& points,
			TransactionInfo* const* transactionInfo,
			const std::vector<ReadConflictRange>& reads,
			const std::vector<int>& offsets,
			bool* transactionConflictStatus,
			std::vector<uint8_t>& conflicts,
			int64_t& tooOldWithinWindow) {

	// One dispatch per resident task per round separates a hint from its use.
	// Local continuations can run in the same visit; scans yield at leaf boundaries.
	enum class Step : std::uint8_t {
		Empty,             // Unused slot.
		InspectNode,       // Inspect a hinted node and initialize its search bounds.
		CompareKeys,       // Consume hints for a separator and midpoint read begin.
		CompareLeafKey,    // Consume hints for a leaf entry and first read begin.
		FindPrev,          // Inspect a hinted left leaf for a live predecessor.
		CheckPrev,         // Compare the hinted predecessor end and read begin.
		StartScan,         // Initialize unresolved reads at the saved scan position.
		ScanLeaf,          // Scan unresolved reads against the current leaf.
		Done               // Completed; retired at round end.
	};
	struct Task {
		NodeRef node{nullptr};
		uint32_t opBegin = 0;   // Assigned mixed slice of readWriteBegins, end exclusive.
		uint32_t opEnd = 0;
		int32_t lo = 0;         // Internal node or leaf search bounds.
		int32_t hi = 0;
		union {
			uint32_t opLo = 0;  // Internal partition search bounds over reads and writes.
			uint32_t readReachedEnd; // Index in readWriteBegins ending the reached-begin prefix.
		};
		uint32_t opHi = 0;
		uint32_t readIndex = 0; // Read being scanned.
		uint32_t readHead = 0;  // First pending read; opEnd is the sentinel.
		uint32_t readPrev = 0;  // Previous list index; opEnd denotes the head.
		uint32_t rank = 0;      // Leaf entry being scanned.
		LeafBlock* prevLeaf = nullptr;
		uint32_t prevRank = 0;
		BNodeHeader* splitCandidate = nullptr;
		Step step = Step::Empty;
		bool rightCheckRequired = false;
	};
	static_assert(G > 0);
	Task tasks[G]{};
	uint16_t order[G]{};
	uint32_t active = 0;
	bool completedTasks = false;
	FreeSlots<G> freeSlots;
	std::vector<Task> pending; // Lowest priority first; back() resumes next.
	// Task slices own disjoint sets of read indices, even when their scans
	// share a leaf. Suspension preserves those links and the task's cursors.
	struct NewTask {
		uint32_t parent; // Position in this round's order, not a physical slot.
		Task task;
	};
	std::vector<NewTask> newTasks;
	newTasks.reserve(G); // At most one right sibling per active task.

	clearWriteDestinations();
	conflicts.assign(reads.size(), 0);
	readWriteBegins.clear();
	nextBegin.clear();
	nextOther.clear();
	writeLeaves.clear();
	writeLeafBuffer_.clear();
	writeLeafBufferHead_ = 0;
	writeLeafBufferFirst_ = writeLeafBufferNext_ = 0;
	readWriteBegins.reserve(points.size() / 2);
	nextBegin.reserve(points.size() / 2);
	nextOther.reserve(points.size() / 2);
	writeLeaves.reserve(points.size() / 2);

	auto buildBeginIndexes = [&]() {
		if (points.size() >= RangeEntry::WriteFlag) {
			throw std::length_error("TempConflictBTree: the batch has too many endpoints");
		}
		const uint32_t pointsSize = static_cast<uint32_t>(points.size());
		uint32_t lastRead = pointsSize;
		uint32_t lastWrite = pointsSize;
		for (uint32_t p = 0; p < pointsSize; ++p) {
			const KeyInfo& point = points[p];
			if (!point.begin) continue;
			const uint32_t t = point.transaction;
			TransactionInfo& info = *transactionInfo[t];
			if (info.tooOld) continue;
			if (!info.readRanges.empty() &&
				reads[offsets[t]].version < reclaimCV_) {
				info.tooOld = true;
				transactionConflictStatus[t] = true;
				++tooOldWithinWindow;
				continue;
			}
			uint32_t rangeIndex;
			if (point.write) {
				rangeIndex = p | RangeEntry::WriteFlag;
			} else {
				const uint32_t r =
					(reinterpret_cast<const char*>(point.pIndex) -
					 reinterpret_cast<const char*>(info.readRanges.begin())) /
					sizeof(*info.readRanges.begin());
				rangeIndex = offsets[t] + r;
				const ReadConflictRange& read = reads[rangeIndex];
				if (transactionConflictStatus[t] &&
					read.conflictingKeyRange == nullptr) continue;
				if (read.begin.compare(read.end) >= 0) continue;
			}
			const uint32_t index = readWriteBegins.size();
			readWriteBegins.push_back({rangeIndex, t});
			nextBegin.push_back(pointsSize);
			nextOther.push_back(pointsSize);
			uint32_t& last = point.write ? lastWrite : lastRead;
			if (last != pointsSize) nextBegin[last] = index;
			// Entries between the previous begin of this type and this one have
			// the opposite type. Consecutive begins of the same type skip the loop.
			for (uint32_t i = last == pointsSize ? 0 : last + 1;
				 i < index; ++i) {
				nextOther[i] = index;
			}
			last = index;
		}
		const uint32_t end = readWriteBegins.size();
		if (lastRead != pointsSize) nextBegin[lastRead] = end;
		if (lastWrite != pointsSize) nextBegin[lastWrite] = end;
		const uint32_t tailBegin =
			lastRead == pointsSize || lastWrite == pointsSize
				? 0 : std::min(lastRead, lastWrite) + 1;
		for (uint32_t i = tailBegin; i < end; ++i) {
			nextOther[i] = end;
		}
	};

	auto markConflict = [&](uint32_t readIndex) {
		const RangeEntry& entry = readWriteBegins[readIndex];
		conflicts[entry.rangeIndex] = 1;
		transactionConflictStatus[entry.transaction] = true;
	};

	auto readAt = [&](uint32_t rank) -> const ReadConflictRange& {
		return reads[readWriteBegins[rank].rangeIndex];
	};
	auto beginAt = [&](uint32_t rank) -> const StringRef& {
		const RangeEntry& entry = readWriteBegins[rank];
		if (!entry.isWrite()) {
			return reads[entry.index()].begin;
		}
		return points[entry.index()].key;
	};

	auto readStillNeeded = [&](uint32_t rank) -> bool {
		const RangeEntry& entry = readWriteBegins[rank];
		if (transactionInfo[entry.transaction]->tooOld ||
			conflicts[entry.rangeIndex]) return false;
		return !transactionConflictStatus[entry.transaction] ||
			reads[entry.rangeIndex].conflictingKeyRange != nullptr;
	};

	// Read ends are not sorted, so a resolved read can be anywhere in the list.
	// Unlink it without moving the begin-sorted records or changing routing bounds.
	auto removeScanRead = [&](Task& task) {
		assert(task.readIndex < task.opEnd);
		const uint32_t next =
			std::min(nextBegin[task.readIndex], task.opEnd);
		assert(next > task.readIndex);
		if (task.readPrev == task.opEnd) {
			task.readHead = next;
		} else {
			nextBegin[task.readPrev] = next;
		}
		task.readIndex = next;
	};

	// Compact before the first saved rank in this batch. With a fixed cutoff
	// and no writes, later visits preserve saved ranks. Empty leaves remain
	// linked and allocated until every traversal cursor has been retired.
	auto cleanReadLeaf = [&](LeafBlock* leaf) {
		if (leaf->lastCleanReadBatch == readBatch_) return;
		cleanExpired(leaf);
		leaf->lastCleanReadBatch = readBatch_;
		if (leaf->header.count == 0) {
			pending_.emplace_back(&leaf->header, leaf->header.generation);
		}
	};

	auto initializeRootTask = [&]() {
		Task& task = tasks[0];
		task.node.header = root_;
		task.opBegin = task.opLo = 0;
		task.opEnd = task.opHi = readWriteBegins.size();
		task.step = Step::InspectNode;

		order[0] = 0;
		freeSlots.unsetFree(0);
		active = 1;
	};

	auto prefetchBlock = [](NodeRef node) {
		const char* bytes = reinterpret_cast<const char*>(node.header);
		for (std::size_t offset = 0; offset < BlockSize; offset += 64) {
			__builtin_prefetch(bytes + offset);
		}
	};

	// Check generation validity before reading any linked-block header.
	// Locate the requested key bytes past headers and issue at most two hints;
	// locating a key in later blocks can still require several header reads.
	auto prefetchStoredKeySlow = [&](const RangeRef& range,
							 std::size_t start, std::size_t length) {
		if (length == 0 || !keyStore_.isValid(range)) return;
		const KeyBlockHeader* block = range.firstBlock;
		std::size_t offset = range.keyOffset;
		while (start >= BlockSize - offset) {
			start -= BlockSize - offset;
			block = block->nextBlock;
			offset = sizeof(KeyBlockHeader);
		}
		offset += start;

		for (unsigned line = 0; line < 2; ++line) {
			const char* bytes = reinterpret_cast<const char*>(block) + offset;
			__builtin_prefetch(bytes);
			const std::size_t lineBytes =
				64 - (reinterpret_cast<std::uintptr_t>(bytes) & 63);
			const std::size_t consumed =
				std::min({length, lineBytes, BlockSize - offset});
			length -= consumed;
			if (length == 0 || line == 1) return;
			offset += consumed;
			if (offset == BlockSize) {
				block = block->nextBlock;
				offset = sizeof(KeyBlockHeader);
			}
		}
	};

	// Requires live referenced bytes throughout the access. If the hinted
	// lines fit in the first block, compute addresses without reading its header.
	// At most two lines are hinted; the rest of a long key may still miss.
	auto prefetchStoredKey = [&](const RangeRef& range,
							 std::size_t start, std::size_t length) {
		if (length == 0) return;
		const std::size_t offset = range.keyOffset + start;
		if (offset < BlockSize) {
			const std::size_t lineBytes = 64 - (offset & 63);
			const char* bytes =
				reinterpret_cast<const char*>(range.firstBlock) + offset;
			if (length <= lineBytes) {
				__builtin_prefetch(bytes);
				return;
			}
			if (offset + lineBytes < BlockSize) {
				__builtin_prefetch(bytes);
				__builtin_prefetch(bytes + lineBytes);
				return;
			}
		}
		prefetchStoredKeySlow(range, start, length);
	};

	auto prefetchRangeBegin = [&](const RangeRef& range) {
		prefetchStoredKey(range, 0, range.beginLength);
	};

	auto prefetchRangeEnd = [&](const RangeRef& range) {
		prefetchStoredKey(range, range.endStart, range.endLength);
	};

	auto prefetchSepKey = [&](NodeRef node, std::size_t rank) {
		const auto slot = node.internal->order[rank];
		prefetchRangeBegin(node.internal->separators[slot]);
	};

	auto prefetchKey = [](const StringRef& key) {
		if (key.empty()) return;
		const auto* bytes = key.begin();
		__builtin_prefetch(bytes);
		const std::size_t lineBytes =
			64 - (reinterpret_cast<std::uintptr_t>(bytes) & 63);
		if (static_cast<std::size_t>(key.size()) > lineBytes) {
			__builtin_prefetch(bytes + lineBytes);
		}
	};

	auto prefetchReadKey = [&](uint32_t rank) {
		prefetchKey(readAt(rank).begin);
	};

	// Continuations issue hints and set the state that consumes them on a
	// later visit. Prefetch completion is not guaranteed.
	auto toInspectNode = [&](Task& task) {
		prefetchBlock(task.node);
		task.step = Step::InspectNode;
	};

	auto toCompareKeys = [&](Task& task) {
		auto* node = task.node.internal;
		const int32_t mid = std::midpoint(task.lo, task.hi);
		const auto slot = node->order[mid];
		if (node->separatorsCV[slot] <= reclaimCV_ &&
			node->separatorsCV[slot] != EmptyChildCV) {
			// Repair the descriptor before hinting: its old block may be reused.
			// Deferred structural cleanup preserves saved internal ranks.
			refreshSeparatorDeferred(node, mid);
		}
		if (node->separatorsCV[slot] != EmptyChildCV) {
			prefetchSepKey(task.node, mid);
		}
		prefetchKey(beginAt(std::midpoint(task.opLo, task.opHi)));
		task.step = Step::CompareKeys;
	};

	auto toCompareLeafKey = [&](Task& task) {
		const auto* leaf = task.node.leaf;
		const int32_t mid = std::midpoint(task.lo, task.hi);
		task.rank = static_cast<uint32_t>(mid);
		prefetchRangeBegin(leaf->ranges[leaf->order[mid]]);
		prefetchReadKey(task.readHead);
		task.step = Step::CompareLeafKey;
	};

	auto toCheckPrev = [&](Task& task) {
		if (task.readIndex == task.readHead) {
			const auto slot = task.prevLeaf->order[task.prevRank];
			prefetchRangeEnd(task.prevLeaf->ranges[slot]);
		}
		prefetchReadKey(task.readIndex);
		task.step = Step::CheckPrev;
	};

	auto toScanLeaf = [&](Task& task) {
		prefetchBlock(task.node);
		task.step = Step::ScanLeaf;
	};

	// Suspension may outlast cache residency. Reissue the resumed state's
	// hints without assuming the earlier data is still cached.
	auto prefetchAgain = [&](Task& task) {
		switch (task.step) {
			case Step::InspectNode:
			case Step::ScanLeaf:
				prefetchBlock(task.node);
				break;
			case Step::CompareKeys:
				toCompareKeys(task);
				break;
			case Step::CompareLeafKey:
				toCompareLeafKey(task);
				break;
			case Step::FindPrev:
				prefetchBlock(NodeRef{.leaf = task.prevLeaf});
				break;
			case Step::CheckPrev:
				toCheckPrev(task);
				break;
			default: break;
		}
	};

	// Separators [lo, hi) delimit child ranks [lo - 1, hi - 1], inclusive.
	// With no boundary left, the entire slice of operations belongs to one child.
	auto assignChildren = [&](Task& task, int32_t lo, int32_t hi,
						  uint32_t opBegin, uint32_t opEnd) {
		task.opBegin = task.opLo = opBegin;
		task.opEnd = task.opHi = opEnd;
		if (lo == hi) {
			const auto slot = task.node.internal->order[lo - 1];
			task.node.header = task.node.internal->children[slot];
			toInspectNode(task);
			return;
		}
		task.lo = lo;
		task.hi = hi;
		toCompareKeys(task);
	};

	// prevRank is exclusive: decrement before inspecting an entry.
	// UINT32_MAX starts at the cleaned count of a newly reached left leaf.
	auto findPrev = [&](Task& task) {
		auto* leaf = task.prevLeaf;
		cleanReadLeaf(leaf);
		if (task.prevRank == UINT32_MAX) {
			task.prevRank = leaf->header.count;
		}
		while (task.prevRank > 0) {
			--task.prevRank;
			const auto slot = leaf->order[task.prevRank];
			if (leaf->cv[slot] > reclaimCV_) {
				toCheckPrev(task);
				return;
			}
		}
		task.prevLeaf = leaf->left;
		if (task.prevLeaf == nullptr) {
			task.step = Step::StartScan;
			return;
		}
		prefetchBlock(NodeRef{.leaf = task.prevLeaf});
		task.prevRank = UINT32_MAX;
		task.step = Step::FindPrev;
	};

	constexpr uint32_t BufferedLeafFlag = uint32_t{1} << 31;
	uint32_t operationCount = 0;
	uint32_t writeFrontier = 0;
	auto bufferTaskLeaf = [&](Task& task, LeafBlock* leaf, uint32_t firstWrite) {
		if (firstWrite == task.opEnd) return;
		WriteLeaf group{leaf, firstWrite, task.opEnd, task.splitCandidate};
		if (task.rightCheckRequired || leaf == nullptr || leaf->header.count == 0) {
			group.requireRightCheck();
		}
		const uint64_t ticket = bufferWriteLeaf(group);
		assert(ticket < BufferedLeafFlag);
		// Read positions retain their next-write links. The first write of
		// each disjoint slice names its buffered result once it reaches a leaf.
		nextOther[firstWrite] = BufferedLeafFlag | static_cast<uint32_t>(ticket);
	};
	auto inspectNode = [&](Task& task) {
		if (task.node.header->kind == BNodeKind::Leaf) {
			auto* leaf = task.node.leaf;
			cleanReadLeaf(leaf);
			if (needsSplit(leaf)) task.splitCandidate = &leaf->header;
			const uint32_t first = task.opBegin;
			const uint32_t other =
				std::min(nextOther[first], task.opEnd);
			const bool writeFirst = readWriteBegins[first].isWrite();
			const uint32_t firstWrite = writeFirst ? first : other;
			task.readHead = writeFirst ? other : first;
			bufferTaskLeaf(task, leaf, firstWrite);
			if (task.readHead == task.opEnd) {
				task.step = Step::Done;
				completedTasks = true;
				return;
			}
			task.hi = leaf->header.count;
			task.lo = 0;
			task.readIndex = task.readHead;
			if (task.hi == 0) {
				task.rank = 0;
				task.prevLeaf = leaf;
				task.prevRank = 0;
				findPrev(task);
			} else {
				toCompareLeafKey(task);
			}
			return;
		}
		task.hi = task.node.header->count;
		if (task.hi == 0) {
			const uint32_t first = task.opBegin;
			const uint32_t firstWrite = readWriteBegins[first].isWrite()
				? first : std::min(nextOther[first], task.opEnd);
			bufferTaskLeaf(task, nullptr, firstWrite);
			task.step = Step::Empty;
			completedTasks = true;
			return;
		}
		if (needsSplit(task.node.header)) task.splitCandidate = task.node.header;
		assignChildren(task, 1, task.hi,
					   task.opBegin, task.opEnd);
	};

	auto compareKeys = [&](Task& task, uint32_t position) {
		// Lower-bound the operation begins: equality belongs on the right, including
		// all duplicates, so an equal comparison cannot end the search early.
		const int32_t mid = std::midpoint(task.lo, task.hi);
		const uint32_t midRead = std::midpoint(task.opLo, task.opHi);
		const uint32_t slot = task.node.internal->order[mid];
		const RangeRef& separator = task.node.internal->separators[slot];
		if (task.node.internal->separatorsCV[slot] == EmptyChildCV) {
			// Empty markers need no live key reference. Route left so forward
			// scans retain overlaps beyond even a wholly empty subtree.
			task.opLo = task.opHi;
			task.rightCheckRequired = true;
		} else if (keyStore_.compareBegin(beginAt(midRead), separator) < 0) {
			task.opLo = midRead + 1;
		} else {
			task.opHi = midRead;
		}
		if (task.opLo < task.opHi) {
			toCompareKeys(task);
			return;
		}
		const uint32_t split = task.opLo;
		if (split == task.opEnd) {
			assignChildren(task, task.lo, mid, task.opBegin, task.opEnd);
		} else if (split == task.opBegin) {
			assignChildren(task, mid + 1, task.hi, task.opBegin, task.opEnd);
		} else {
			// Hint the sibling's first access at creation. It cannot execute
			// in this round; admission may suspend it for later rounds.
			Task right = task;
			assignChildren(right, mid + 1, task.hi, split, task.opEnd);
			assignChildren(task, task.lo, mid, task.opBegin, split);
			newTasks.push_back({position, right});
		}
	};

	auto compareLeafKey = [&](Task& task) {
		const int32_t mid = static_cast<int32_t>(task.rank);
		const auto* leaf = task.node.leaf;
		const auto slot = leaf->order[mid];
		const int cmp = keyStore_.compareBegin(
			readAt(task.readHead).begin, leaf->ranges[slot]);
		if (cmp == 0) {
			task.rank = mid;
			task.readIndex = task.readHead;
			task.step = Step::StartScan;
			return;
		}
		if (cmp > 0) {
			task.lo = mid + 1;
		} else {
			task.hi = mid;
		}
		if (task.lo < task.hi) {
			toCompareLeafKey(task);
			return;
		}
		task.rank = task.lo;
		task.readIndex = task.readHead;
		task.prevLeaf = task.node.leaf;
		task.prevRank = task.rank;
		findPrev(task);
	};

	auto checkPrev = [&](Task& task) {
		if (readStillNeeded(task.readIndex)) {
			const auto* leaf = task.prevLeaf;
			const auto slot = leaf->order[task.prevRank];
			const auto& read = readAt(task.readIndex);
			const int cmp = keyStore_.compareEnd(
				read.begin, leaf->ranges[slot]);
			if (cmp >= 0) {
				// Later reads also begin at or after this write's end.
				task.step = Step::StartScan;
				return;
			}
			if (leaf->cv[slot] > read.version) {
				markConflict(task.readIndex);
			}
		}
		task.readIndex=std::min(nextBegin[task.readIndex],task.opEnd);
		while (task.readIndex < task.opEnd &&
			   !readStillNeeded(task.readIndex)) {
			task.readIndex=std::min(nextBegin[task.readIndex],task.opEnd);
		}
		if (task.readIndex < task.opEnd) {
			toCheckPrev(task);
		} else {
			task.step = Step::StartScan;
		}
	};

	// Scan a whole leaf per visit, without dispatching individual pairs.
	// Yield at a leaf boundary after hinting the next leaf.
	auto scanLeaf = [&](Task& task) {
		auto* leaf = task.node.leaf;
		cleanReadLeaf(leaf);
		while (task.rank < leaf->header.count) {
			const auto slot = leaf->order[task.rank];
			const auto& write = leaf->ranges[slot];
			while (task.readIndex < task.opEnd) {
				if (!readStillNeeded(task.readIndex)) {
					removeScanRead(task);
					continue;
				}
				const auto& read = readAt(task.readIndex);
				if (task.readIndex >= task.readReachedEnd) {
					if (keyStore_.compareEnd(read.begin, write) >= 0) {
						// This read and the following ones need later writes.
						break;
					}
					// Ordered disjoint writes have increasing ends; a reached
					// begin stays below every later write end.
					task.readReachedEnd = task.readIndex + 1;
				}
				if (keyStore_.compareBegin(read.end, write) <= 0) {
					removeScanRead(task);
				} else if (leaf->cv[slot] > read.version) {
					markConflict(task.readIndex);
					removeScanRead(task);
				} else {
					task.readPrev = task.readIndex;
					task.readIndex =
						std::min(nextBegin[task.readIndex], task.opEnd);
				}
			}
			if (task.readHead == task.opEnd) {
				task.step = Step::Done;
				completedTasks = true;
				return;
			}
			++task.rank;
			task.readIndex = task.readHead;
			task.readPrev = task.opEnd;
		}
		if (leaf->right == nullptr) {
			task.step = Step::Done;
			completedTasks = true;
			return;
		}
		task.node.leaf = leaf->right;
		task.rank = 0;
		toScanLeaf(task);
	};

	auto startScan = [&](Task& task) {
		// Partition and scan phases are exclusive. The union now holds the
		// reached prefix of begin-sorted reads instead of a partition bound.
		task.readReachedEnd = task.opBegin;
		task.readIndex = task.readHead;
		task.readPrev = task.opEnd;
		while (task.readIndex < task.opEnd &&
			   !readStillNeeded(task.readIndex)) {
			removeScanRead(task);
		}

		if (task.readIndex == task.opEnd) {
			task.step = Step::Done;
			completedTasks = true;
			return;
		}

		scanLeaf(task);
	};

	// The visitation order stays fixed throughout a round. New siblings
	// cannot execute until a later round.
	auto runRound = [&]() {
		for (uint32_t t = 0; t < active; ++t) {
			Task& task = tasks[order[t]];
			switch (task.step) {
				case Step::InspectNode: {
					inspectNode(task);
					break;
				}
				case Step::CompareKeys: {
					compareKeys(task, t);
					break;
				}
				case Step::CompareLeafKey: {
					compareLeafKey(task);
					break;
				}
				case Step::FindPrev: {
					findPrev(task);
					break;
				}
				case Step::CheckPrev: {
					checkPrev(task);
					break;
				}
				case Step::StartScan: {
					startScan(task);
					break;
				}
				case Step::ScanLeaf: {
					scanLeaf(task);
					break;
				}
				default: break;
			}
		}
	};

	// Follow only write-bearing slices. A read-only slice cannot block output;
	// its nextOther link jumps directly to the next write begin.
	auto flushWritePrefix = [&]() {
		while (writeFrontier < operationCount) {
			const uint32_t result = nextOther[writeFrontier];
			if ((result & BufferedLeafFlag) == 0) break;
			const uint32_t next = emitWriteLeaf(result & ~BufferedLeafFlag);
			assert(next > writeFrontier && next <= operationCount);
			writeFrontier = next == operationCount || readWriteBegins[next].isWrite()
				? next : nextOther[next];
		}
	};
	auto endRound = [&]() {
		flushWritePrefix();
		// Bounds and cursors may change without changing the task order.
		// Rebuild only when tasks finish or a partition creates a sibling.
		if (!completedTasks && newTasks.empty()) return;

		// Merge survivors and right siblings in execution order. Values below
		// G identify resident slots; the rest identify entries in newTasks.
		// Only indices are staged: selected resident tasks keep their slots.
		std::array<uint16_t, 2 * G> roundOrder{};
		uint32_t count = 0;
		uint32_t created = 0;
		for (uint32_t t = 0; t < active; ++t) {
			const auto slot = order[t];
			if (tasks[slot].step == Step::Empty || tasks[slot].step == Step::Done) {
				tasks[slot].step = Step::Empty;
				freeSlots.setFree(slot);
			} else {
				roundOrder[count++] = slot;
			}
			if (created < newTasks.size() && newTasks[created].parent == t) {
				roundOrder[count++] = static_cast<uint16_t>(G + created);
				++created;
			}
		}
		assert(created == newTasks.size());
		const uint32_t selected = std::min<uint32_t>(G, count);

		// Suspend the suffix in reverse order so the first excluded task
		// resumes first, ahead of older pending work. Free its resident slots
		// before admitting any selected new tasks.
		for (uint32_t i = count; i > selected; --i) {
			const auto index = roundOrder[i - 1];
			if (index < G) {
				pending.push_back(tasks[index]);
				tasks[index].step = Step::Empty;
				freeSlots.setFree(index);
			} else {
				pending.push_back(newTasks[index - G].task);
			}
		}
		for (uint32_t i = 0; i < selected; ++i) {
			const auto index = roundOrder[i];
			if (index < G) {
				order[i] = index;
			} else {
				assert(freeSlots.any());
				const auto slot = static_cast<uint16_t>(freeSlots.first());
				tasks[slot] = newTasks[index - G].task;
				freeSlots.unsetFree(slot);
				order[i] = slot;
			}
		}
		newTasks.clear();
		active = selected;

		while (active < G && !pending.empty()) {
			assert(freeSlots.any());
			const auto slot = static_cast<uint16_t>(freeSlots.first());
			tasks[slot] = pending.back();
			pending.pop_back();
			prefetchAgain(tasks[slot]);
			freeSlots.unsetFree(slot);
			order[active++] = slot;
		}
		completedTasks = false;
	};

	++readBatch_;
	buildBeginIndexes();
	if (readWriteBegins.size() >= BufferedLeafFlag) {
		throw std::length_error("TempConflictBTree: the batch has too many operations");
	}
	operationCount = static_cast<uint32_t>(readWriteBegins.size());
	firstWriteBegin_ = operationCount;
	if (operationCount != 0) {
		firstWriteBegin_ = readWriteBegins[0].isWrite() ? 0 : nextOther[0];
	}
	writeFrontier = firstWriteBegin_;
	if (firstWriteBegin_ == operationCount) {
		writeFrontier = operationCount;
	} else {
		writeLeafBuffer_.reserve(2 * static_cast<std::size_t>(G));
	}
	writeLeavesSplitEpoch_ = writeSplitEpoch_;
	if (root_ == nullptr || readWriteBegins.empty()) return;
	initializeRootTask();
	prefetchBlock(tasks[0].node);
	while (active != 0) {
		runRound();
		endRound();
	}
	assert(writeFrontier == operationCount);
	assert(writeLeafBufferFirst_ == writeLeafBufferNext_);
	prepareWriteDestinations();
	cleanPending();
}

// Explicit instantiations for the supported task capacities.
template void TempConflictBTree::checkReads<16>(
	const std::vector<KeyInfo>& points,
	TransactionInfo* const* transactionInfo,
	const std::vector<ReadConflictRange>& reads,
	const std::vector<int>& offsets,
	bool* transactionConflictStatus,
	std::vector<uint8_t>& conflicts,
	int64_t& tooOldWithinWindow);

template void TempConflictBTree::checkReads<24>(
	const std::vector<KeyInfo>& points,
	TransactionInfo* const* transactionInfo,
	const std::vector<ReadConflictRange>& reads,
	const std::vector<int>& offsets,
	bool* transactionConflictStatus,
	std::vector<uint8_t>& conflicts,
	int64_t& tooOldWithinWindow);

template void TempConflictBTree::checkReads<32>(
	const std::vector<KeyInfo>& points,
	TransactionInfo* const* transactionInfo,
	const std::vector<ReadConflictRange>& reads,
	const std::vector<int>& offsets,
	bool* transactionConflictStatus,
	std::vector<uint8_t>& conflicts,
	int64_t& tooOldWithinWindow);
