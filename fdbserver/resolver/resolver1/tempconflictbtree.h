// Temporal Conflict B+ tree
//
// Stores write-conflict history ordered by key, with commit versions (CVs)
// identifying the age of retained conflict information.
//
// Uses BNodeStore for node allocation and age tracking, and KeyStore for key
// storage. Both stores draw blocks from the same PagePool.
//
// Checks batches of read-conflict intervals and applies batches of accepted
// write-conflict intervals. Reads share grouped traversal; writes are
// applied individually.
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
// Breadth-first midpoint ranks of binary partitions over [0, count).
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
}
} // namespace temp_conflict_btree_detail

class TempConflictBTree {
	friend struct TempConflictBTreeTestAccess;
public:
	explicit TempConflictBTree(std::size_t budget);
	~TempConflictBTree() = default;

	TempConflictBTree(const TempConflictBTree&) = delete;
	TempConflictBTree& operator=(const TempConflictBTree&) = delete;

	// Requires begin-sorted endpoints and input records/bytes valid for the
	// call. No writes or key-block reclamation may interleave with traversal.
	// Resets conflicts and sets conflicting transaction flags without clearing
	// existing flags. Reads below reclaimCV_ mark the transaction too old and
	// increment tooOldWithinWindow once per newly too-old transaction.
	// A non-null reporting pointer keeps other reads of a conflicted
	// transaction eligible; individual conflict bits identify their results.
	// May refresh separators, clean expired entries and remove empty nodes.
	// Does not add writes or reclaim key blocks.
	template<uint8_t G>
	void checkReads(const std::vector<KeyInfo>& points,
					TransactionInfo* const* transactionInfo,
					const std::vector<ReadConflictRange>& reads,
					const std::vector<int>& offsets,
					bool* transactionConflictStatus,
					std::vector<uint8_t>& conflicts,
					int64_t& tooOldWithinWindow);
	// Writes are sorted, disjoint intervals from accepted transactions.
	// All writes share cv, which must be nondecreasing between calls.
	// Before each write, reclaims until the pool has room for it in the
	// worst case; throws std::length_error if it cannot, even when empty.
	void addWrites(
		const std::span<const std::pair<StringRef, StringRef>> writes,
		Version cv);

	Version reclaimCV() const {
		return reclaimCV_;
	}

private:
	PagePool pool_;
	KeyStore keyStore_;
	BNodeStore nodeStore_;

	BNodeHeader* root_ = nullptr;
	Version reclaimCV_ = 0;
	// The longest key addWrite has stored. Exteriors combine stored keys
	// with a write's keys, so it bounds the room a write may need.
	std::size_t maxKeyLength_ = 0;
	uint64_t readBatch_ = 0;
	std::vector<std::pair<BNodeHeader*,std::uint64_t>> 
		pending_;	// Deferred leaf cleanup records; generations reject released nodes.
	void cleanPending();

	struct RangeEntry {
		uint32_t rangeIndex;  // Global index in reads; local index in writeRanges.
		uint32_t transaction; // Index in transactionInfo.
	};
	std::vector<RangeEntry> readBegins;
	std::vector<RangeEntry> writeBegins;

	// Covers both internal separators and leaf entries for the configured block size.
	static constexpr std::size_t MaxSeparators =
		std::max(InternalCapacity, LeafCapacity);
	// Generated at compile time. Bounds are [lo, hi).
	static constexpr auto prefetchOrder =
		temp_conflict_btree_detail::makePrefetchOrder<MaxSeparators>();
	
	// Returns whether key exists; leaf/rank identify its entry or insertion
	// point. An empty tree returns leaf == nullptr. nodeNeedsSplit is the
	// deepest node on the path still needing a split, or nullptr.
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
		const std::pair<StringRef, StringRef>& write,
		Version cv);
	bool hasConflict(const ReadConflictRange& read);
	// Inserts range at leaf/rank, preserving overlapping exteriors
	// and their CVs. Requires two free slots. Returns true if the write
	// is complete. Otherwise leaves leaf/rank at the inserted range.
	// An expired previous range is erased without reading its keys.
	bool insertWithLeftExterior(
		const std::pair<StringRef, StringRef>& write,
		const RangeRef& range,
		Version cv,
		bool found,
		LeafBlock*& leaf,
		std::size_t& rank);
	// Erases expired entries without reading their keys, and live entries
	// ending at or before end. Stops at the first live entry ending after
	// end, or tree end.
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
	// Requires reclaimed key blocks to have CV <= reclaimCV_. Refreshes
	// rank without structural removal; queues leaf cleanup and preserves
	// saved internal ranks. Returns whether that child has a live entry.
	// Empty runs receive EmptyChildCV and may borrow a live reference,
	// preferring the left. With no live child, all references remain unread.
	// Empty markers must route conservatively left, without key comparison;
	// a read can extend beyond an empty subtree into later live history.
	bool refreshSeparatorDeferred(InternalBlock* node, std::size_t rank);
	// Returns the first live range/CV, or nullopt; defers cleanup.
	// Refreshes separators and marks leading empty children.
	std::optional<std::pair<const RangeRef*, Version>> firstLiveKey(
		BNodeHeader* node);
	// Refreshes from the child's first live key, deferring cleanup.
	// Returns false without replacing the separator if no live key exists.
	bool refreshLiveSeparator(InternalBlock* node, std::size_t rank);
	// Copies the separator at rank from into ranks [first, last) and
	// marks those ranks EmptyChildCV. Requires a live source reference.
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
	void removeSubtreeRec(BNodeHeader* node);};
