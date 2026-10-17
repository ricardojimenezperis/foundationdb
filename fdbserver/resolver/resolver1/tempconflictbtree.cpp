#include "tempconflictbtree.h"

TempConflictBTree::TempConflictBTree(std::size_t budget)
	: pool_(budget, BlockSize),
	  keyStore_(pool_),
	  nodeStore_(pool_, keyStore_) {
}

void TempConflictBTree::checkReads(
		std::span<const ReadConflictRange> reads,
		bool* transactionConflictStatus) const {

}

void TempConflictBTree::addWrites(
		const std::span<const std::pair<StringRef, StringRef>> writes,
		Version cv) {
	std::size_t sz = writes.size();
	for (std::size_t i=0; i<sz; ++i) {
		addWrite(writes[i], cv);
	}
}

void TempConflictBTree::addWrite(
	const std::pair<StringRef, StringRef>& write,
	Version cv) {

	constexpr std::size_t MaxWriteBlocks = 35;
	reclaimUntilAvail(MaxWriteBlocks);

	LeafBlock* leaf = nullptr;
	std::size_t rank = 0;
	const bool found = findKey(write.first, leaf, rank);

	if (leaf == nullptr) {
		auto range = keyStore_.appendRange(write.first, write.second, cv);
		leaf = nodeStore_.allocateLeaf(cv);
		nodeStore_.insertAt(std::size_t{0}, *range, cv, leaf);
		root_ = &leaf->header;
		return;
	}

	std::size_t slot = 0;
	bool hasPrevious = false;
	if (!found) {
		hasPrevious = prevRank(leaf, rank, slot);
	}

	auto rangeInt =
		keyStore_.appendRange(write.first, write.second, cv);

	if (hasPrevious) {
		const RangeRef oldRef = leaf->ranges[slot];
		const Version oldCV = leaf->cv[slot];

		if (keyStore_.compareEnd(write.first, oldRef) < 0) {
			// Preserve the left exterior with its original CV.
			auto rangeExtLeft =
				keyStore_.appendRange(oldRef, write.first, oldCV);
			leaf->ranges[slot] = *rangeExtLeft;

			if (keyStore_.compareEnd(write.second, oldRef) < 0) {
				// Both exteriors come from the same original interval.
				auto rangeExtRight =
					keyStore_.appendRange(write.second, oldRef, oldCV);
				++rank;
				insertAt(rank, *rangeInt, cv, leaf);
				++rank;
				insertAt(rank, *rangeExtRight, oldCV, leaf);
				return;
			}
		}

		nextRank(leaf, rank, slot);
	}

	// eraseAt leaves the cursor at the next entry or at the end.
	// If the tree becomes empty, it returns leaf == nullptr and rank == 0.
	int comp = 0;
	while (leaf != nullptr && rank < leaf->header.count) {
		slot = leaf->order[rank];
		comp = keyStore_.compareEnd(write.second, leaf->ranges[slot]);
		if (comp < 0) {
			break;
		}
		eraseAt(rank, leaf);
	}

	if (leaf != nullptr && rank < leaf->header.count) {
		slot = leaf->order[rank];

		if (comp < 0 &&
			keyStore_.compareBegin(write.second, leaf->ranges[slot]) > 0) {

			// Preserve the right exterior with its original CV.
			auto rangeExtRight = keyStore_.appendRange(
				write.second, leaf->ranges[slot], leaf->cv[slot]);
			leaf->ranges[slot] = *rangeExtRight;
		}
	}

	if (leaf == nullptr) {
		leaf = nodeStore_.allocateLeaf(cv);
		root_ = &leaf->header;
		rank = 0;
	}

	insertAt(rank, *rangeInt, cv, leaf);
}

bool TempConflictBTree::findKey(
		const StringRef& key,
		LeafBlock*& leaf,
		std::size_t& rank) const {

	leaf = nullptr;
	rank = 0;

	if (root_ == nullptr) {
		return false;
	}

	BNodeHeader* current = root_;
	while (current->kind == BNodeKind::Internal) {
		const auto* internal = reinterpret_cast<const InternalBlock*>(current);
		const bool found = nodeStore_.findKey(key, rank, internal);
		if (!found && rank > 0) {
			--rank;
		}
		current = internal->children[internal->order[rank]];
	}

	leaf = reinterpret_cast<LeafBlock*>(current);
	return nodeStore_.findKey(key, rank, leaf);
}

bool TempConflictBTree::hasConflict(const ReadConflictRange& read) const {
	LeafBlock* leaf = nullptr;
	std::size_t rank = 0;
	const bool found = findKey(read.begin, leaf, rank);

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
		if (slotPresent &&
				keyStore_.compareEnd(read.begin, leaf->ranges[slot]) < 0 &&
			leaf->cv[slot] > read.version) {
			return true;
		}
		if (slotPresent) {
			slotPresent = nextRank(leaf, rank, slot);
		} else if (rank < leaf->header.count) {
			slot = leaf->order[rank];
			slotPresent = true;
		} else {
			// The current leaf has no candidate; look to the right.
			slotPresent = nextRank(leaf, rank, slot);
		}
	}

	while (slotPresent &&
			keyStore_.compareBegin(read.end, leaf->ranges[slot]) > 0) {
		if (leaf->cv[slot] > read.version) {
			return true;
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
	while (next != nullptr && next->header.count == 0) {
		next = next->right;
	}

	if (next == nullptr) {
		rank = leaf->header.count;
		return false;
	}

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
	while (previous != nullptr && previous->header.count == 0) {
		previous = previous->left;
	}

	if (previous == nullptr) {
		return false;
	}

	leaf = previous;
	rank = leaf->header.count - 1;
	slot = leaf->order[rank];
	return true;
}

// Inserts range at the given logical rank, splitting nodes as needed.
// On return, leaf and rank identify the inserted entry, even if a split
// moved it to another leaf. rank is a position in order, not a physical slot.
void  TempConflictBTree::insertAt(
	std::size_t& rank,
	const RangeRef& range,
	Version cv,
	LeafBlock*& leaf) {

}

// Removes the entry at the given logical rank and repairs the tree.
// On return, leaf and rank identify the next entry, or the end position
// (rank == count) in the last leaf.
// If the tree becomes empty, leaf is nullptr and rank is zero.
void  TempConflictBTree::eraseAt(std::size_t& rank, LeafBlock*& leaf) {

}

void  TempConflictBTree::reclaimOldest() {

}

void  TempConflictBTree::reclaimUntilAvail(int n) {
	
}
