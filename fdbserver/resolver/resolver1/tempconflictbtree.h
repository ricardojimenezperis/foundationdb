// Temporal Conflict B+ tree
//
// Stores write-conflict history ordered by key, with commit versions (CVs)
// identifying the age of retained conflict information.
//
// Uses BNodeStore for node allocation and age tracking, and KeyStore for key
// storage. Both stores draw blocks from the same PagePool.
//
// Checks batches of read-conflict intervals and applies batches of accepted
// write-conflict intervals. This version processes intervals individually.
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

#include <cstddef>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

class TempConflictBTree {
	friend struct TempConflictBTreeTestAccess;
public:
	explicit TempConflictBTree(std::size_t budget);
	~TempConflictBTree() = default;

	TempConflictBTree(const TempConflictBTree&) = delete;
	TempConflictBTree& operator=(const TempConflictBTree&) = delete;

	// Reads must have passed the caller's admission check against reclaimCV.
	// Marks conflicting transactions without clearing existing status flags.
	// Does not modify history or reclaim storage.
	void checkReads(
		std::span<const ReadConflictRange> reads,
		bool* transactionConflictStatus);

	// Writes are sorted, disjoint intervals from accepted transactions.
	// All writes share cv, which must be nondecreasing between calls.
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

	// Returns the insertion point, leaf & rank, and whether is found.
	// If the tree is empty return nullptr as leaf.
	bool findKey(
		const StringRef& key,
		LeafBlock*& leaf,
		std::size_t& rank,
	    BNodeHeader*& nodeNeedsSplit
	);
	// The deepest node on the path of findKey that needs a split: leaf if it
	// does, otherwise the deepest of candidates, the internal nodes that
	// needed one on the way down, that still does after cleaning. Consumes
	// candidates. Returns nullptr if there is none.
	BNodeHeader* findSplitCandidate(
		LeafBlock* leaf,
		std::vector<InternalBlock*>& candidates);
	void addWrite(
		const std::pair<StringRef, StringRef>& write,
		Version cv);
	bool hasConflict(const ReadConflictRange& read);
	// Helpers of addWrite, in the order it calls them. leaf and rank are the
	// write's position: from findKey, after any split.
	//
	// Inserts range with cv at that position, in that leaf, which needs room
	// for two entries. Unless found, keeps the left exterior of the previous
	// range, with its original CV, when it overlaps the start of the write;
	// the previous range may be in the leaf before, and is trimmed in place.
	// If the write lies inside it, also inserts its right exterior right
	// after the write and returns true: the write is done. Otherwise returns
	// false with the position at the write. An expired previous range is
	// erased, unread, after the insertion.
	bool insertWithLeftExterior(
		const std::pair<StringRef, StringRef>& write,
		const RangeRef& range,
		Version cv,
		bool found,
		LeafBlock*& leaf,
		std::size_t& rank);
	// Erases the entries from the position on whose end is not after end. The
	// write is in the tree before the position, so the tree cannot become
	// empty. Leaves the position at the first entry ending after end, or at
	// the end position.
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
	// Inserts range at the given logical rank, splitting nodes as needed.
	// On return, leaf and rank identify the inserted entry, even if a split
	// moved it to another leaf. rank is a position in order, not a physical slot.
	void insertAt(
		std::size_t& rank,
		const RangeRef& range,
		Version cv,
		LeafBlock*& leaf);
	// Removes the entry at the given logical rank and repairs the tree.
	// On return, leaf and rank identify the next entry, or the end position
	// (rank == count) in the last leaf.
	// If the tree becomes empty, leaf is nullptr and rank is zero.
	void eraseAt(std::size_t& rank, LeafBlock*& leaf);
	// Detaches the child at rank from node and repairs the tree as eraseAt
	// does for an entry: split counts, separators, ancestors left empty and
	// the root. The subtree is not released; it is returned without parent,
	// its leaves taken out of the leaf chain. Requires a non-empty subtree.
	BNodeHeader* eraseAt(std::size_t rank, InternalBlock* node);
	// Takes the run of leaves from first to last out of the leaf chain,
	// linking its neighbours to each other. The run keeps its inner links;
	// its ends are left without neighbours.
	static void unlinkLeaves(LeafBlock* first, LeafBlock* last);
	static void unlinkLeaf(LeafBlock* leaf) {
		unlinkLeaves(leaf, leaf);
	}
	// The first and last leaves of the subtree under node.
	static LeafBlock* firstLeaf(BNodeHeader* node);
	static LeafBlock* lastLeaf(BNodeHeader* node);
	// Advances to the next entry. The leaf to the right, if any, is not empty:
	// only the root leaf may be.
	// Requires leaf != nullptr and rank <= leaf->header.count.
	// Returns true if an entry is found, updating leaf, rank and its slot.
	// Otherwise, returns false, keeps leaf unchanged, and sets rank to count.
	bool nextRank(LeafBlock*& leaf, std::size_t& rank, std::size_t& slot) const;
	// Moves to the previous entry. The leaf to the left, if any, is not empty.
	// Requires leaf != nullptr and rank <= leaf->header.count.
	// rank == count is accepted as an insertion position after the last entry.
	// Returns true if an entry is found, updating leaf, rank and its slot.
	// Otherwise, returns false and leaves both parameters unchanged.
	bool prevRank(LeafBlock*& leaf, std::size_t& rank, std::size_t& slot) const;	
	// Reclamation must keep the tree invariant that only the root leaf may be
	// empty: leaves it empties, and internal nodes left without children, are
	// removed before it returns.
	//
	// Reclaims the oldest age: the smallest CV among the oldest node and the
	// oldest key block. reclaimCV_ advances to it, and every node and key
	// block with exactly that CV is reclaimed, a node with its subtree.
	// Returns false, changing nothing, if there is nothing to reclaim.
	bool reclaimOldest();
	// Reclaims ages until the pool has at least n available blocks. Returns
	// false if there is nothing left to reclaim before reaching n.
	bool reclaimUntilAvail(std::size_t n);
	// Removes expired entries without releasing the leaf itself.
	// The leaf may be left empty, which only the root leaf may stay: the
	// caller must then remove it before the operation returns.
	// Returns true if any entry was removed.
	bool cleanExpired(LeafBlock* leaf);
	// Refreshes every expired separator of node with refreshSeparator.
	// Returns true if any separator was refreshed or any child removed.
	bool refreshSeparators(InternalBlock* node);
	// Refreshes the separator at rank if it has expired (its CV is not newer
	// than reclaimCV_). Expired entries of the child are removed, and the
	// separator becomes the child's first key with its CV. If the child
	// becomes empty, it is removed: rank then refers to the next child, and
	// node may become empty. Leaf cursors held by the caller may become
	// invalid. Requires reclaimCV_ to have advanced at least to the maximum
	// CV of every reclaimed key block, so that a separator whose bytes were
	// reclaimed is always expired. Returns true if anything changed.
	bool refreshSeparator(InternalBlock* node, std::size_t rank);
	// Removes and releases an empty child.
	// Repairs leaf links when the child is a leaf.
	// Does not release parent, even if it becomes empty.
	void removeEmptyChild(InternalBlock* parent, std::size_t rank);
	// An internal node needs a split when every free slot may be taken by
	// the split of a child that needs one.
	static bool needsSplit(const InternalBlock* node) {
		return node->header.childrenNeedingSplit == node->freeSlotCount();
	}
	// A leaf needs a split when it has at most one free slot, since a write
	// can add two entries to a leaf: the written range and the right part of
	// the range it splits.
	static bool needsSplit(const LeafBlock* node) {
		return node->freeSlotCount() <= 1;
	}
	static bool needsSplit(const BNodeHeader* node) {
		return node->kind == BNodeKind::Leaf
			? needsSplit(reinterpret_cast<const LeafBlock*>(node))
			: needsSplit(reinterpret_cast<const InternalBlock*>(node));
	}
	// Propagates the changes of node to its ancestors in a single ascent,
	// stopping when no change remains:
	// - splitDelta is +1 when node has just started needing a split and -1
	//   when it has just stopped. Each parent's count changes by splitDelta,
	//   which carries on only while the parent changes state too.
	// - updateSeparator sets the separator of node in its parent to the first
	//   key of node, with its CV, and carries on while node is the first child
	//   of its parent. Requires node not to be empty.
	// - updateCV raises each parent's bound for node to node's maximum CV,
	//   and carries on while the parent's maximum CV rises.
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
	// The first key of node, with its CV: the separator node should have in
	// its parent. Requires node not to be empty.
	std::pair<KeyRef, Version> minSeparator(const BNodeHeader* node) const;
	// Moves the upper part of oldNode, cut by weight, to a new node and
	// returns it. Moved children point to the new node and are counted there.
	InternalBlock* splitInternal(InternalBlock* oldNode, Version cv);
	// Moves the upper half of leaf to a new leaf linked after it and returns
	// it. Adjusts leaf and rank as split does.
	LeafBlock* splitLeaf(LeafBlock*& leaf, std::size_t& rank, bool found,
						 Version cv);
	// Puts sibling right after node in node's parent, creating a new root if
	// node has none. Requires neither of them to need a split; the parent
	// then keeps its own state.
	void insertSibling(BNodeHeader* node, BNodeHeader* sibling, Version cv);
	// Splits node, updating the tree and split counters.
	// If node is leaf, adjusts leaf and rank to preserve the position
	// returned by findKey; found is the value findKey returned. A key not
	// found at the cut rank stays at the end of the old leaf, since it is
	// below the first key of the new one.
	void split(BNodeHeader* node, LeafBlock*& leaf, std::size_t& rank,
			   bool found, Version cv);
	void normalizeRoot();
	// Detaches node from the tree, or empties the tree if node is the root,
	// and releases node and every node below it.
	void removeSubtree(BNodeHeader* node);
	// Releases node and every node below it, children first.
	void removeSubtreeRec(BNodeHeader* node);
};
