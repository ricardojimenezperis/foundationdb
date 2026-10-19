#include "tempconflictbtree.h"

TempConflictBTree::TempConflictBTree(std::size_t budget)
	: pool_(budget, BlockSize),
	  keyStore_(pool_),
	  nodeStore_(pool_, keyStore_) {
}

void TempConflictBTree::checkReads(
		std::span<const ReadConflictRange> reads,
		bool* transactionConflictStatus) {

	for (const auto& read : reads) {
		if (!transactionConflictStatus[read.transaction]) {
			transactionConflictStatus[read.transaction] = hasConflict(read);
		}
	}
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
	BNodeHeader* nodeNeedsSplit = nullptr;
	const bool found = findKey(write.first, leaf, rank, nodeNeedsSplit);
	if (leaf == nullptr) {
		// empty tree
		auto range = keyStore_.appendRange(write.first, write.second, cv);
		leaf = nodeStore_.allocateLeaf(cv);
		nodeStore_.insertAt(std::size_t{0}, *range, cv, leaf);
		root_ = &leaf->header;
		return;
	}
	if (nodeNeedsSplit!=nullptr){
		split(nodeNeedsSplit, leaf, rank);
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

	int comp = 0;
	while (leaf != nullptr && rank < leaf->header.count) {
		slot = leaf->header.order[rank];
		comp = keyStore_.compareEnd(write.second, leaf->ranges[slot]);
		if (comp < 0) {
			break;
		}
		// remove overlapping ranges
		eraseAt(rank, leaf);
	}
	if (leaf != nullptr && rank < leaf->header.count) {
		slot = leaf->header.order[rank];
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
		current = internal->children[internal->header.order[rank]];
	}

	leaf = reinterpret_cast<LeafBlock*>(current);
	cleanExpired(leaf);
	if (!leaf->header.hasSpace()) {
		nodeNeedsSplit=current;
	} else {
		while (!needSplit.empty()){
			InternalBlock* internal =needSplit.back();
			needSplit.pop_back();
			if (needsSplit(internal)){
				nodeNeedsSplit=&internal->header;
				break;
			}
		}
	}
	return nodeStore_.findKey(key, rank, leaf);
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
		slot = leaf->header.order[rank];
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
			slot = leaf->header.order[rank];
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
		slot = leaf->header.order[rank];
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
	slot = leaf->header.order[rank];
	return true;
}

bool TempConflictBTree::prevRank(
		LeafBlock*& leaf,
		std::size_t& rank,
		std::size_t& slot) const {
	if (rank > 0) {
		--rank;
		slot = leaf->header.order[rank];
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
	slot = leaf->header.order[rank];
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

void TempConflictBTree::propagateSplitCleared(BNodeHeader* node) {
	while (node->parent != nullptr) {
		BNodeHeader* parent = node->parent;
		auto* internal = reinterpret_cast<InternalBlock*>(parent);
		const bool splitBefore = needsSplit(internal);

		assert(parent->childrenNeedingSplit > 0);
		--parent->childrenNeedingSplit;

		if (!splitBefore || needsSplit(internal)) {
			break;
		}
		node = parent;
	}
}

bool TempConflictBTree::refreshSeparator(
		InternalBlock* node, std::size_t rank) {
	const auto slot = node->header.order[rank];
	if (node->separatorsCV[slot] > reclaimCV_) {
		return false;
	}

	if (node->children[slot]->kind == BNodeKind::Internal) {
		auto* child =
			reinterpret_cast<InternalBlock*>(node->children[slot]);

		// Removing the first grandchild exposes the next one, whose
		// separator may also have expired.
		while (child->header.count > 0 &&
			   child->separatorsCV[child->header.order[0]] <= reclaimCV_) {
			refreshSeparator(child, 0);
		}

		if (child->header.count == 0) {
			removeEmptyChild(node, rank);
		} else {
			const auto slot0 = child->header.order[0];
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
			const auto slot0 = child->header.order[0];
			node->separatorsCV[slot] = child->cv[slot0];
			node->separators[slot] =
				keyStore_.beginKey(child->ranges[slot0]);
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

bool TempConflictBTree::cleanExpired(LeafBlock* leaf) {
	const bool splitBefore = !leaf->header.hasSpace();
	bool deleted = false;
	std::size_t rank = 0;

	while (rank < leaf->header.count) {
		const auto slot = leaf->header.order[rank];
		if (leaf->cv[slot] <= reclaimCV_) {
			nodeStore_.eraseAt(rank, leaf);
			deleted = true;
		} else {
			++rank;
		}
	}

	if (splitBefore && leaf->header.hasSpace()) {
		propagateSplitCleared(&leaf->header);
	}
	return deleted;
}

void TempConflictBTree::removeEmptyChild(
		InternalBlock* parent, std::size_t rank) {
	const auto slot = parent->header.order[rank];
	BNodeHeader* child = parent->children[slot];
	assert(child->count == 0);

	const bool splitBefore = needsSplit(parent);

	if (child->kind == BNodeKind::Leaf) {
		auto* leaf = reinterpret_cast<LeafBlock*>(child);
		if (leaf->left != nullptr) {
			leaf->left->right = leaf->right;
		}
		if (leaf->right != nullptr) {
			leaf->right->left = leaf->left;
		}
	}

	// The empty child no longer needs splitting. Its transition
	// was already propagated when it was cleaned.
	nodeStore_.eraseAt(rank, parent);
	nodeStore_.releaseNode(child);

	if (splitBefore && !needsSplit(parent)) {
		propagateSplitCleared(&parent->header);
	}
}

void TempConflictBTree::split(BNodeHeader* node, LeafBlock*& leaf, std::size_t& rank){
	int cutRank=0;
	if (node->kind==BNodeKind::Internal) {
		// reclaimCV_ is a tradeoff to avoid reordering that would be n log(n)
		InternalBlock* newNode= nodeStore_.allocateInternal(reclaimCV_);
		InternalBlock* oldNode= reinterpret_cast<InternalBlock*> (node);
		newNode->header.parent=oldNode->header.parent;
		int toMoveWeight=(oldNode->header.count+node->freeSlotCount())/2;
		int currentRank=0;
		std::size_t  currentSlot=oldNode->header.order[currentRank];
		// compute cut point
		while (toMoveWeight>0) {
			if (oldNode->children[currentSlot]->childrenNeedingSplit
					== oldNode->children[currentSlot]->freeSlotCount()) {
				toMoveWeight-=2;
			} else {
				--toMoveWeight;
			}
			++currentRank;
			currentSlot=oldNode->header.order[currentRank];
		}
		cutRank=currentRank;
		while (currentRank<=newNode->header.count) {
			if (oldNode->children[currentSlot]->childrenNeedingSplit
					== oldNode->children[currentSlot]->freeSlotCount()) {
				toMoveWeight-=2;
				++newNode->header.childrenNeedingSplit;
			} else {
				--toMoveWeight;
			}
			newNode->header.order[newNode->header.count]=newNode->header.count;
			newNode->separators[newNode->header.count]=oldNode->separators[currentSlot];
			newNode->separatorsCV[newNode->header.count]=oldNode->separatorsCV[currentSlot];
			newNode->children[newNode->header.count]=oldNode->children[currentSlot];
			newNode->maxSubtreesCV[newNode->header.count]=oldNode->maxSubtreesCV[currentSlot];
			newNode->header.unsetFree(newNode->header.count);
			newNode->children[newNode->header.count]->parent=
				reinterpret_cast<BNodeHeader*> (newNode);
			newNode->children[newNode->header.count]->parentSlot=newNode->header.count;
			nodeStore_.eraseAt(currentRank,oldNode);
			++newNode->header.count;
			++currentRank;
			currentSlot=oldNode->header.order[currentRank];
			size_t lastToDel=oldNode->header.count;
			// Remove from cutRank to cound in oldNode
			for (size_t rank=cutRank; rank < oldNode->header.count; ++rank) {
				reinterpret_cast<BNodeHeader*>(oldNode)->setFree(oldNode->header.order[rank]);
			}
			oldNode->header.count=cutRank;
			// find the first empty parent slot
			
			// store pointer to new node and update related counts
			newNode->header.parentSlot=;
		}
	} else {

	}
}