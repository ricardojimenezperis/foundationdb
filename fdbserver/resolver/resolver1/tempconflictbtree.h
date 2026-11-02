// Temporal Conflict B+ tree
//
// Stores write-conflict history ordered by key, with commit versions (CVs)
// identifying the age of retained conflict information.
//
// Uses BNodeStore for node allocation and age tracking, and KeyStore for key
// storage. Both stores draw blocks from the same PagePool.
//
// Routes sorted read and write begins together, checks read conflicts, and
// applies accepted writes in groups prepared for their destination leaves.
// Input key bytes are borrowed during each call; retained keys are copied
// into KeyStore.
//
// Manages the root, searches, updates, splits, separators, parent-child links,
// and links between leaves. Entries remain in physical slots; permutations
// maintain their logical key order.
//
// Only the root leaf may be empty when an operation returns. A leaf emptied
// during an operation is removed, with any internal node left without
// children, before the operation returns.
//
// Reclamation advances a commit-version cutoff (reclaimCV). Expired leaf
// entries are removed during local cleanup, and expired internal separators
// are refreshed before use. Entire expired subtrees can be detached and
// released without deleting each entry through a separate root traversal.
//
// Coordinates reclamation of tree nodes and key blocks when space is needed.
// Node maximum CVs are conservative upper bounds and need not decrease when
// entries are removed.
//
// No reclamation may occur while key bytes are being accessed.

#pragma once

#include "pagepool.h"
#include "keystore.h"
#include "bnodestore.h"
#include "fdbcompat.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace temp_conflict_btree_detail {
// Preorder of binary partitions over [0, count) in breadth order:
// 		midpoint, left, right.
// Row count contains count valid ranks; the remaining positions are unused.
template<std::size_t Capacity>
constexpr auto makePrefetchOrder() {
	std::array<std::array<std::uint16_t, Capacity>, Capacity + 1> table{};
	struct Interval {
		std::size_t lo;
		std::size_t hi;
	};
	for (std::size_t count = 1; count <= Capacity; ++count) {
		std::array<Interval, Capacity> queue{};
		std::size_t head = 0;
		std::size_t tail = 0;
		std::size_t position = 0;
		queue[tail++] = {0, count};
		while (head < tail) {
			const auto [lo, hi] = queue[head++];
			const std::size_t mid = lo + (hi - lo) / 2;
			table[count][position++] = static_cast<std::uint16_t>(mid);
			if (lo < mid) queue[tail++] = {lo, mid};
			if (mid + 1 < hi) queue[tail++] = {mid + 1, hi};
		}
	}
	return table;
} // template
} //namespace

class TempConflictBTree {
	friend struct TempConflictBTreeTestAccess;
public:
	explicit TempConflictBTree(std::size_t budget);
	~TempConflictBTree() = default;

	TempConflictBTree(const TempConflictBTree&) = delete;
	TempConflictBTree& operator=(const TempConflictBTree&) = delete;

	// Checks reads and prepares write destinations from sorted batch endpoints.
	// Marks conflicting and too-old transactions without clearing status flags.
	// May clean expired entries; does not add writes or advance reclaimCV.
	template<uint8_t G>
	void checkReads(const std::vector<KeyInfo>& points,
					TransactionInfo* const* transactionInfo,
					const std::vector<ReadConflictRange>& reads,
					const std::vector<int>& offsets,
					bool* transactionConflictStatus,
					std::vector<uint8_t>& conflicts,
					int64_t& tooOldWithinWindow);
	// Coalesces overlapping or adjacent accepted writes from checkReads. The batch,
	// its indices and input key bytes must remain valid through this call.
	// All intervals use cv; commit versions must be nondecreasing across calls.
	// May reclaim history. Throws std::length_error if the per-write reserve
	// cannot be met; earlier writes and reclamation are not rolled back.
	void addWrites(
		const std::vector<KeyInfo>& points,
		TransactionInfo* const* transactionInfo,
		const bool* transactionConflictStatus,
		Version cv);

	Version reclaimCV() const {
		return reclaimCV_;
	}

private:
	PagePool pool_;
	KeyStore keyStore_;
	BNodeStore nodeStore_;

	BNodeHeader* root_ = nullptr;
	LeafBlock* rightmostLeaf_ = nullptr; // Direct insertion anchor for the right-end sentinel.
	std::size_t maxKeyLength_ = 0;
	Version reclaimCV_ = 0;
	// Cleanup stamp: advances for each read batch and when writes advance the cutoff.
	std::uint64_t readBatch_ = 0;
	Version cleanLeafCV_ = 0;
	std::vector<std::pair<BNodeHeader*,std::uint64_t>>
		pending_;	// leaves to clean, with their generation, queued by refreshSeparatorDeferred
	void cleanPending();

	struct RangeEntry {
		static constexpr uint32_t WriteFlag = uint32_t{1} << 31;
		uint32_t rangeIndex;  // Tagged index: global in reads, begin endpoint in points.
		uint32_t transaction; // Index in transactionInfo.
		bool isWrite() const { return (rangeIndex & WriteFlag) != 0; }
		uint32_t index() const { return rangeIndex & ~WriteFlag; }
		void setWrite() { rangeIndex |= WriteFlag; }
	};
	std::vector<RangeEntry> readWriteBegins;
	std::vector<uint32_t> nextBegin; // Next begin of the same type; reads may be unlinked.
	// First later begin of the other type. A write-bearing slice replaces
	// its first write position with a tagged buffered-result ticket.
	std::vector<uint32_t> nextOther;
	struct WriteLeaf {
		static constexpr uint32_t RightCheckFlag = uint32_t{1} << 31;
		LeafBlock* leaf;      // Prepared destination, repaired locally before release.
		uint32_t firstWrite;  // First write in the assigned mixed slice.
		uint32_t opEnd;       // Exclusive bound with an optional RightCheckFlag; use end().
		BNodeHeader* splitCandidate = nullptr; // Deepest candidate observed during descent.
		uint32_t end() const { return opEnd & ~RightCheckFlag; }
		bool needsRightCheck() const { return (opEnd & RightCheckFlag) != 0; }
		void requireRightCheck() { opEnd |= RightCheckFlag; }
	};
	static_assert(sizeof(void*) != 8 || sizeof(WriteLeaf) == 24);
	std::vector<WriteLeaf> writeLeaves;
	uint32_t firstWriteBegin_ = 0;
	struct BufferedWriteLeaf {
		WriteLeaf group;
		bool released;
	};
	std::vector<BufferedWriteLeaf> writeLeafBuffer_;
	std::size_t writeLeafBufferHead_ = 0;
	uint64_t writeLeafBufferFirst_ = 0;
	uint64_t writeLeafBufferNext_ = 0;
	// Tickets survive circular-buffer growth and do not occupy task slots.
	uint64_t bufferWriteLeaf(const WriteLeaf& group);
	uint32_t emitWriteLeaf(uint64_t ticket);
	// After routing, nextOther[firstWrite] holds a group representative. Leaf
	// firstWrite fields identify representatives; no node layout changes.
	bool writeDestinationsActive_ = false;
	void prepareWriteDestinations();
	void clearWriteDestinations();
	uint32_t resolveWriteGroup(uint32_t group);
	void redirectWriteLeaf(LeafBlock* leaf, LeafBlock* replacement);
	void redirectWriteSubtree(BNodeHeader* node, LeafBlock* replacement);
	// Split candidates remain reusable until split state or topology changes.
	uint64_t writeSplitEpoch_ = 0;
	uint64_t writeLeavesSplitEpoch_ = 0;
	struct WriteCursor {
		LeafBlock* leaf = nullptr;
		BNodeHeader* splitCandidate = nullptr;
		uint64_t splitEpoch = 0;
		bool rightCheckRequired = true; // False for a proved partition or the current right end.
	};
	WriteCursor* activeWriteCursor_ = nullptr;

	// Covers both internal separators and leaf entries for the configured block size.
	static constexpr std::size_t MaxSeparators =
		std::max(InternalCapacity, LeafCapacity);
	// Generated at compile time. Bounds are [lo, hi).
	static constexpr auto prefetchOrder =
		temp_conflict_btree_detail::makePrefetchOrder<MaxSeparators>();

	// Returns the insertion point, leaf & rank, and whether is found.
	// If the tree is empty return nullptr as leaf.
	bool findKey(
		const StringRef& key,
		LeafBlock*& leaf,
		std::size_t& rank,
	    BNodeHeader*& nodeNeedsSplit
	);
	// Returns the deepest node still needing a split, or nullptr.
	BNodeHeader* findSplitCandidate(
		LeafBlock* leaf,
		std::vector<InternalBlock*>& candidates);
	void addWrite(
		WriteCursor& cursor,
		const std::pair<StringRef, StringRef>& write,
		Version cv);
	bool hasConflict(const ReadConflictRange& read);
	// Inserts range at leaf/rank, preserving overlapping exteriors
	// and their CVs. Requires two free slots. Returns true if the write
	// is complete. Otherwise leaves leaf/rank at the inserted range.
	// Removes an expired predecessor without reading its key bytes.
	bool insertWithLeftExterior(
		const std::pair<StringRef, StringRef>& write,
		const RangeRef& range,
		Version cv,
		bool found,
		LeafBlock*& leaf,
		std::size_t& rank);
	// Erases entries ending at or before end.
	// Stops at the first entry ending after end, or tree end.
	// The preceding write keeps the tree nonempty.
	void eraseCoveredRanges(
		const StringRef& end,
		LeafBlock*& leaf,
		std::size_t& rank);
	// If the entry at the position strictly contains end, replaces its range
	// with [end, its end), keeping its CV. Does nothing without an entry.
	void preserveRightExterior(
		const StringRef& end,
		LeafBlock* leaf,
		std::size_t rank);
	// Requires an empty tree. Installs a new leaf, still empty, as the root.
	LeafBlock* createRootLeaf(Version cv);
	// Inserts range at logical rank, splitting as needed.
	// Updates leaf/rank to identify the inserted entry.
	void insertAt(
		std::size_t& rank,
		const RangeRef& range,
		Version cv,
		LeafBlock*& leaf);
	// Removes the entry or child at logical rank and repairs the tree.
	// Updates leaf/rank to the next entry or tree end (rank == count).
	// If empty, sets leaf to nullptr and rank to zero.
	void eraseAt(std::size_t& rank, LeafBlock*& leaf);
	// Returns the nonempty subtree, detached but not released.
	// Clears its parent and unlinks its leaves from the leaf chain.
	BNodeHeader* eraseAt(std::size_t rank, InternalBlock* node);
	// Unlinks [first, last], joining its neighbours.
	// Preserves inner links and clears outer links.
	static void unlinkLeaves(LeafBlock* first, LeafBlock* last);
	static void unlinkLeaf(LeafBlock* leaf) {
		unlinkLeaves(leaf, leaf);
	}
	// The first and last leaves of the subtree under node.
	static LeafBlock* firstLeaf(BNodeHeader* node);
	static LeafBlock* lastLeaf(BNodeHeader* node);
	// Moves to the next/previous entry, updating leaf/rank/slot.
	// Requires leaf != nullptr and rank <= count.
	// On failure, nextRank sets rank to count; prevRank changes nothing.
	bool nextRank(LeafBlock*& leaf, std::size_t& rank, std::size_t& slot) const;
	bool prevRank(LeafBlock*& leaf, std::size_t& rank, std::size_t& slot) const;
	// Reclaims the oldest node/key age, advancing reclaimCV_.
	// Releases whole subtrees and removes empty nonroot nodes.
	// Returns false without changes if nothing can be reclaimed.
	bool reclaimOldest();
	// Reclaims until n blocks are available; returns false if exhausted.
	bool reclaimUntilAvail(std::size_t n);
	// Removes expired entries; returns whether any were removed.
	// Caller must remove the leaf if empty and nonroot.
	bool cleanExpired(LeafBlock* leaf);
	// Refreshes expired separators; returns whether anything changed.
	bool refreshSeparators(InternalBlock* node);
	// Refreshes an expired separator, removing its child if empty.
	// May invalidate cursors. Returns whether anything changed.
	// Requires reclaimed key blocks to have CV <= reclaimCV_.
	bool refreshSeparator(InternalBlock* node, std::size_t rank);
	// Child has no live entries; removal is deferred.
	static constexpr Version EmptyChildCV = -1;
	// Refreshes without removal; queues cleanup. Same cutoff requirement.
	// Returns whether the child has live entries.
	// If empty, copies a live neighbour's separator, preferring left,
	// with EmptyChildCV; searches must redirect to that neighbour.
	// If all children are empty, separators must not be compared.
	bool refreshSeparatorDeferred(InternalBlock* node, std::size_t rank);
	// Returns the first live range/CV, or nullopt; defers cleanup.
	// Refreshes separators and marks leading empty children.
	std::optional<std::pair<const RangeRef*, Version>> firstLiveKey(
		BNodeHeader* node);
	// Refreshes from the child's first live key, deferring cleanup.
	// Returns false without replacing the separator if no live key exists.
	bool refreshLiveSeparator(InternalBlock* node, std::size_t rank);
	// Copies separator from to [first, last), marking them EmptyChildCV.
	static void markEmptyChildren(InternalBlock* node, std::size_t first,
								  std::size_t last, std::size_t from);
	// Releases an empty child and repairs leaf links.
	// Keeps parent even if empty.
	void removeEmptyChild(InternalBlock* parent, std::size_t rank);
	// Removes an empty node and ancestors left empty; normalizes root.
	// An empty root leaf may remain.
	void removeIfEmpty(BNodeHeader* node);
	// All free slots are reserved for pending child splits.
	static bool needsSplit(const InternalBlock* node) {
		return node->header.childrenNeedingSplit == node->freeSlotCount();
	}
	// A write may need two slots: its range and a right exterior.
	static bool needsSplit(const LeafBlock* node) {
		return node->freeSlotCount() <= 1;
	}
	static bool needsSplit(const BNodeHeader* node) {
		return node->kind == BNodeKind::Leaf
			? needsSplit(reinterpret_cast<const LeafBlock*>(node))
			: needsSplit(reinterpret_cast<const InternalBlock*>(node));
	}
	// Propagates split state, first key and maximum CV changes upward.
	// splitDelta: +1 starts needing a split; -1 stops; 0 unchanged.
	// Requires a nonempty node when updating its separator.
	// Stops when no change remains.
	void propagateUpdates(BNodeHeader* node, int splitDelta,
						  bool updateSeparator, bool updateCV);
	void propagateUpdates(LeafBlock* node, int splitDelta,
						  bool updateSeparator, bool updateCV) {
		propagateUpdates(&node->header, splitDelta, updateSeparator, updateCV);
	}
	void propagateUpdates(InternalBlock* node, int splitDelta,
						  bool updateSeparator, bool updateCV) {
		propagateUpdates(&node->header, splitDelta, updateSeparator, updateCV);
	}
	// Returns the first range and CV. Requires a nonempty node.
	std::pair<const RangeRef*, Version> minSeparator(
		const BNodeHeader* node) const;
	// Splits by weight; returns the upper part with child links repaired.
	InternalBlock* splitInternal(InternalBlock* oldNode, Version cv);
	// Splits the upper half into a new right leaf.
	// Adjusts leaf/rank as split does.
	LeafBlock* splitLeaf(LeafBlock*& leaf, std::size_t& rank, bool found,
						 Version cv);
	// Inserts sibling after node, creating a root if needed.
	// Neither node may need a split; preserves the parent's split state.
	void insertSibling(BNodeHeader* node, BNodeHeader* sibling, Version cv);
	// Splits node and updates split counters.
	// Preserves the findKey position in leaf/rank.
	// An unfound key at the cut stays at the end of the left leaf.
	void split(BNodeHeader* node, LeafBlock*& leaf, std::size_t& rank,
			   bool found, Version cv);
	void normalizeRoot();
	// Detaches and releases the subtree; clears root_ if it is the root.
	void removeSubtree(BNodeHeader* node);
	// Releases the subtree, children first.
	void removeSubtreeRec(BNodeHeader* node);
};
