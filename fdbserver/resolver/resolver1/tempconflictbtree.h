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
#include <utility>

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
		bool* transactionConflictStatus) const;

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
	void addWrite(
		const std::pair<StringRef, StringRef>& write,
		Version cv);
	bool hasConflict(const ReadConflictRange& read) const;
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
	// Advances to the next entry, skipping empty leaves.
	// Requires leaf != nullptr and rank <= leaf->header.count.
	// Returns true if an entry is found, updating leaf, rank and its slot.
	// Otherwise, returns false, keeps leaf unchanged, and sets rank to count.
	bool nextRank(LeafBlock*& leaf, std::size_t& rank, std::size_t& slot) const;
	// Moves to the previous entry, skipping empty leaves.
	// Requires leaf != nullptr and rank <= leaf->header.count.
	// rank == count is accepted as an insertion position after the last entry.
	// Returns true if an entry is found, updating leaf, rank and its slot.
	// Otherwise, returns false and leaves both parameters unchanged.
	bool prevRank(LeafBlock*& leaf, std::size_t& rank, std::size_t& slot) const;	
	void reclaimOldest();
	void reclaimUntilAvail(int n);
	// Removes expired entries without releasing the leaf itself.
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
		return node->header.childrenNeedingSplit == std::popcount(node->freeSlots);
	}
	void propagateSplitCleared(BNodeHeader* node);
};

