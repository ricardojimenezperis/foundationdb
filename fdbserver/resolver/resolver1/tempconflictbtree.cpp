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
		split(nodeNeedsSplit, leaf, rank, found, cv);
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
	if (needsSplit(leaf)) {
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

void  TempConflictBTree::insertAt(
		std::size_t& rank,
		const RangeRef& range,
		Version cv,
		LeafBlock*& leaf) {
	const bool splitBefore = needsSplit(leaf);
	nodeStore_.insertAt(rank, range, cv, leaf);
	if (rank == 0) {
		updateMinSeparator(leaf);
	}
	bool updateSplit=!splitBefore && needsSplit(leaf);
	bool updateCV= true;
	BNodeHeader* current=&leaf->header;
	InternalBlock* parent = leaf->parent();
	while (parent!=nullptr && (updateSplit || updateCV)){
		if (updateSplit) {
			const bool parentSplitBefore = needsSplit(parent);
			++parent->header.childrenNeedingSplit;
			updateSplit= !parentSplitBefore && needsSplit(parent); 
		}
		const auto slot = current->parentSlot;
		if (updateCV) {
			if (parent->maxSubtreesCV[slot] < current->maxCV) {
				parent->maxSubtreesCV[slot] = current->maxCV;
			}
			updateCV = parent->header.maxCV < current->maxCV;
			if (updateCV) {
				nodeStore_.updateCV(parent, current->maxCV);
			}
		}
		current=&parent->header;
		parent=current->parent;
	}
}

void TempConflictBTree::normalizeRoot() {
	while (root_ != nullptr) {
		if (root_->count == 0) {
			nodeStore_.releaseNode(root_);
			root_ = nullptr;
			return;
		}
		if (root_->kind != BNodeKind::Internal || root_->count != 1) {
			return;
		}
		auto* oldRoot = reinterpret_cast<InternalBlock*>(root_);
		root_ = oldRoot->children[oldRoot->header.order[0]];
		root_->parent = nullptr;
		root_->parentSlot = 0;
		nodeStore_.releaseNode(&oldRoot->header);
	}
}

// Removes the entry at the given logical rank and repairs the tree.
// On return, leaf and rank identify the next entry, or the end position
// (rank == count) in the last leaf.
// If the tree becomes empty, leaf is nullptr and rank is zero.
void  TempConflictBTree::eraseAt(std::size_t& rank, LeafBlock*& leaf) {
	bool splitBefore=needsSplit(leaf);
	nodeStore_.eraseAt(rank,leaf);
	if (splitBefore && !needsSplit(leaf)) {
		propagateSplitCleared(leaf);
	}
	if (rank==0 && leaf->header.count > 0) {
		updateMinSeparator(leaf);
	}
	if (leaf->header.count==0) {
		if (leaf->parent()==nullptr){
			nodeStore_.releaseNode(leaf);
			root_=nullptr;
			leaf=nullptr;
		} else {
			LeafBlock* oldLeft=leaf->left;
			LeafBlock* oldRight=leaf->right;
			InternalBlock* oldParent=leaf->parent();
			removeEmptyChild(leaf->parent(), leaf->parentRank());
			if (oldLeft==nullptr && oldRight==nullptr) { 
				//last remaining leaf -> empty tree
				while (oldParent!=nullptr){
					InternalBlock* tmp=oldParent;
					oldParent=oldParent->parent();
					nodeStore_.releaseNode(tmp);
				}
				root_=nullptr;
				leaf=nullptr;
			} else {
				if (oldRight!=nullptr) {
					leaf= oldRight;
					rank= 0;
				} else { // oldLeft cannot be nullptr
					leaf= oldLeft;
					rank=leaf->header.count;
				}
				while (oldParent->header.count == 0 && oldParent->parent() != nullptr) {
					InternalBlock* tmp = oldParent;
					oldParent = tmp->parent();
					removeEmptyChild(oldParent, tmp->parentRank());
				}
				if (oldParent->header.count > 0) {
					updateMinSeparator(oldParent);
				}
				normalizeRoot();
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

void  TempConflictBTree::reclaimOldest() {

}

void  TempConflictBTree::reclaimUntilAvail(int n) {
	
}

void TempConflictBTree::propagateSplitCleared(BNodeHeader* node) {
	while (node->parent != nullptr) {
		InternalBlock* parent = node->parent;
		const bool splitBefore = needsSplit(parent);
		assert(parent->header.childrenNeedingSplit > 0);
		--parent->header.childrenNeedingSplit;
		if (!splitBefore || needsSplit(parent)) {
			break;
		}
		node = &parent->header;
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
	const bool splitBefore = needsSplit(leaf);
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

	if (splitBefore && !needsSplit(leaf)) {
		propagateSplitCleared(leaf);
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
		propagateSplitCleared(parent);
	}
}

void TempConflictBTree::split(
		BNodeHeader* node,
		LeafBlock*& leaf,
		std::size_t& rank,
		bool found,
		Version cv) {
	assert(node != nullptr);
	assert(needsSplit(node));
	assert(cv >= node->maxCV);

	InternalBlock* parentNode = node->parent;
	const bool parentSplitBefore =
		parentNode != nullptr && needsSplit(parentNode);

	BNodeHeader* sibling = nullptr;
	KeyRef oldSeparator{};
	KeyRef newSeparator{};
	Version oldSeparatorCV = 0;
	Version newSeparatorCV = 0;

	if (node->kind == BNodeKind::Internal) {
		auto* oldNode = reinterpret_cast<InternalBlock*>(node);

		// Use the write CV as a conservative bound so allocation
		// can append the node to the age list.
		auto* newNode = nodeStore_.allocateInternal(cv);
		assert(newNode != nullptr);

		int remainingWeight =
			(oldNode->header.count +
			 oldNode->header.childrenNeedingSplit) / 2;

		int cutRank = 0;
		while (remainingWeight > 0) {
			const auto slot = oldNode->header.order[cutRank];
			remainingWeight -=
				needsSplit(oldNode->children[slot]) ? 2 : 1;
			++cutRank;
		}

		assert(cutRank > 0);
		assert(cutRank < oldNode->header.count);

		const int oldCount = oldNode->header.count;
		for (int currentRank = cutRank;
			 currentRank < oldCount;
			 ++currentRank) {
			const auto oldSlot = oldNode->header.order[currentRank];
			const auto newSlot = newNode->header.count;
			BNodeHeader* child = oldNode->children[oldSlot];

			newNode->header.order[newSlot] = newSlot;
			newNode->separators[newSlot] =
				oldNode->separators[oldSlot];
			newNode->separatorsCV[newSlot] =
				oldNode->separatorsCV[oldSlot];
			newNode->children[newSlot] = child;
			newNode->maxSubtreesCV[newSlot] =
				oldNode->maxSubtreesCV[oldSlot];
			newNode->header.unsetFree(newSlot);

			child->parent = newNode;
			child->parentSlot = newSlot;

			if (needsSplit(child)) {
				++newNode->header.childrenNeedingSplit;
				--oldNode->header.childrenNeedingSplit;
			}

			oldNode->header.setFree(oldSlot);
			++newNode->header.count;
		}
		oldNode->header.count =
			static_cast<std::uint8_t>(cutRank);

		const auto oldFirst = oldNode->header.order[0];
		const auto newFirst = newNode->header.order[0];

		oldSeparator = oldNode->separators[oldFirst];
		oldSeparatorCV = oldNode->separatorsCV[oldFirst];
		newSeparator = newNode->separators[newFirst];
		newSeparatorCV = newNode->separatorsCV[newFirst];
		sibling = &newNode->header;

		// An internal split leaves leaf and rank unchanged.
	} else {
		auto* oldNode = reinterpret_cast<LeafBlock*>(node);
		assert(leaf == oldNode);

		auto* newNode = nodeStore_.allocateLeaf(cv);
		assert(newNode != nullptr);

		const std::size_t cutRank = oldNode->header.count / 2;
		const std::size_t oldCount = oldNode->header.count;

		assert(cutRank > 0);
		assert(cutRank < oldCount);

		for (std::size_t currentRank = cutRank;
			 currentRank < oldCount;
			 ++currentRank) {
			const auto oldSlot = oldNode->header.order[currentRank];
			const auto newSlot = newNode->header.count;

			newNode->header.order[newSlot] = newSlot;
			newNode->ranges[newSlot] = oldNode->ranges[oldSlot];
			newNode->cv[newSlot] = oldNode->cv[oldSlot];
			newNode->header.unsetFree(newSlot);

			oldNode->header.setFree(oldSlot);
			++newNode->header.count;
		}
		oldNode->header.count =
			static_cast<std::uint8_t>(cutRank);

		newNode->left = oldNode;
		newNode->right = oldNode->right;
		if (newNode->right != nullptr) {
			newNode->right->left = newNode;
		}
		oldNode->right = newNode;

		if (rank > cutRank || (found && rank == cutRank)) {
			leaf = newNode;
			rank -= cutRank;
		}

		const auto oldFirst = oldNode->header.order[0];
		const auto newFirst = newNode->header.order[0];

		oldSeparator = keyStore_.beginKey(oldNode->ranges[oldFirst]);
		oldSeparatorCV = oldNode->cv[oldFirst];
		newSeparator = keyStore_.beginKey(newNode->ranges[newFirst]);
		newSeparatorCV = newNode->cv[newFirst];
		sibling = &newNode->header;
	}

	const bool oldSplitAfter = needsSplit(node);
	const bool newSplitAfter = needsSplit(sibling);

	if (parentNode == nullptr) {
		parentNode = nodeStore_.allocateInternal(cv);
		assert(parentNode != nullptr);

		nodeStore_.insertAt(
			0, oldSeparator, node, oldSeparatorCV, parentNode);
		nodeStore_.insertAt(
			1, newSeparator, sibling, newSeparatorCV, parentNode);

		parentNode->header.childrenNeedingSplit =
			static_cast<std::uint8_t>(oldSplitAfter) +
			static_cast<std::uint8_t>(newSplitAfter);

		root_ = &parentNode->header;
	} else {
		const std::size_t parentRank = node->parentRank();
		assert(parentNode->header.freeSlotCount() > 0);

		nodeStore_.insertAt(
			parentRank + 1,
			newSeparator,
			sibling,
			newSeparatorCV,
			parentNode);

		// The old node needed splitting before redistribution.
		assert(parentNode->header.childrenNeedingSplit > 0);
		--parentNode->header.childrenNeedingSplit;
		parentNode->header.childrenNeedingSplit +=
			static_cast<std::uint8_t>(oldSplitAfter) +
			static_cast<std::uint8_t>(newSplitAfter);

		const bool parentSplitAfter = needsSplit(parentNode);
		assert(parentSplitBefore || !parentSplitAfter);

		if (parentSplitBefore && !parentSplitAfter) {
			propagateSplitCleared(parentNode);
		}
	}

	// Propagate the sibling's conservative maximum CV.
	BNodeHeader* current = sibling;
	while (current->parent != nullptr) {
		InternalBlock* parent = current->parent;
		const auto slot = current->parentSlot;

		if (parent->maxSubtreesCV[slot] < current->maxCV) {
			parent->maxSubtreesCV[slot] = current->maxCV;
		}

		if (parent->header.maxCV >= current->maxCV) {
			break;
		}
		nodeStore_.updateCV(parent, current->maxCV);
		current = &parent->header;
	}
}

void TempConflictBTree::updateMinSeparator(LeafBlock* leaf) {
	assert(leaf->header.count > 0);
	InternalBlock* parent = leaf->parent();
	if (parent == nullptr) {
		return;
	}
	const auto first = leaf->header.order[0];
	const auto slot = leaf->header.parentSlot;
	parent->separators[slot] = keyStore_.beginKey(leaf->ranges[first]);
	parent->separatorsCV[slot] = leaf->cv[first];
	if (parent->header.order[0] == slot) {
		updateMinSeparator(parent);
	}
}

void TempConflictBTree::updateMinSeparator(InternalBlock* node) {
	assert(node->header.count > 0);
	for (InternalBlock* parent = node->parent(); parent != nullptr;
		 node = parent, parent = node->parent()) {
		const auto first = node->header.order[0];
		const auto slot = node->header.parentSlot;
		parent->separators[slot] = node->separators[first];
		parent->separatorsCV[slot] = node->separatorsCV[first];
		if (parent->header.order[0] != slot) {
			return;
		}
	}
}
