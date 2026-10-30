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
	if (nodeNeedsSplit != nullptr) {
		split(nodeNeedsSplit, leaf, rank, found, cv);
	}
	auto range = keyStore_.appendRange(write.first, write.second, cv);
	if (leaf == nullptr) {
		leaf = createRootLeaf(cv);
		insertAt(rank, *range, cv, leaf);
		return;
	}
	if (insertWithLeftExterior(write, *range, cv, found, leaf, rank)) {
		return;
	}
	// The write is in its leaf. From the entry after it on, possibly in later
	// leaves, what it covers is erased and the last range it overlaps keeps
	// its right exterior; moving on does not change where the write went.
	std::size_t slot = 0;
	if (nextRank(leaf, rank, slot)) {
		eraseCoveredRanges(write.second, leaf, rank);
		preserveRightExterior(write.second, leaf, rank);
	}
}

bool TempConflictBTree::insertWithLeftExterior(
		const std::pair<StringRef, StringRef>& write,
		const RangeRef& range,
		Version cv,
		bool found,
		LeafBlock*& leaf,
		std::size_t& rank) {
	// Every insertion of a write goes into the leaf findKey reached, which
	// split left with room for two entries. Other leaves are only trimmed in
	// place or lose entries.
	assert(leaf->freeSlotCount() >= 2);
	LeafBlock* previousLeaf = leaf;
	std::size_t previous = rank;
	std::size_t slot = 0;
	const bool hasPrevious = !found && prevRank(previousLeaf, previous, slot);
	// An expired range is not read: its key may have been reclaimed.
	const bool expired = hasPrevious && previousLeaf->cv[slot] <= reclaimCV_;
	if (hasPrevious && !expired &&
			keyStore_.compareEnd(write.first, previousLeaf->ranges[slot]) < 0) {
		// The previous range overlaps the start of the write: keep its left
		// exterior with its original CV, in its place.
		const RangeRef oldRef = previousLeaf->ranges[slot];
		const Version oldCV = previousLeaf->cv[slot];
		auto left = keyStore_.appendRange(oldRef, write.first, oldCV);
		previousLeaf->ranges[slot] = *left;
		if (keyStore_.compareEnd(write.second, oldRef) < 0) {
			// The write lies inside that range: its right exterior follows the
			// write, in the same leaf, even when the range is in the leaf
			// before. Live entries are disjoint, so the leaf's own entries
			// begin after the range ends.
			auto right = keyStore_.appendRange(write.second, oldRef, oldCV);
			insertAt(rank, range, cv, leaf);
			++rank;
			insertAt(rank, *right, oldCV, leaf);
			return true;
		}
	}
	insertAt(rank, range, cv, leaf);
	if (expired) {
		// Erased only now, so that its leaf, if it is this one, cannot be left
		// empty. Entries before it end before it, so none overlaps the write.
		// eraseAt leaves the position at the next entry: the write.
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
	// The write is already in the tree, before the position, so erasing cannot
	// empty it.
	assert(leaf != nullptr);
	while (rank < leaf->header.count) {
		const auto slot = leaf->order[rank];
		// An expired entry, whose key may be gone, is erased unread.
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
	// Deepest first: candidates were pushed on the way down.
	while (!candidates.empty()) {
		InternalBlock* node = candidates.back();
		candidates.pop_back();
		if (needsSplit(node)) {
			return &node->header;
		}
	}
	return nullptr;
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
		// Checking the CV first keeps the key of an expired entry, which
		// may have been reclaimed, unread: the read is not older than the
		// cut.
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
			// The current leaf has no candidate; look to the right.
			slotPresent = nextRank(leaf, rank, slot);
		}
	}

	while (slotPresent) {
		// Entries in leaves not cleaned on the way may have expired, and
		// their keys been reclaimed: they are skipped unread.
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
	assert(next->header.count > 0);
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

	assert(previous->header.count > 0);
	leaf = previous;
	rank = leaf->header.count - 1;
	slot = leaf->order[rank];
	return true;
}

void  TempConflictBTree::insertAt(
		std::size_t& rank,
		const RangeRef& range,
		Version cv,
		LeafBlock*& leaf) {
	const bool splitBefore = needsSplit(leaf);
	nodeStore_.insertAt(rank, range, cv, leaf);
	const bool splitAfter = needsSplit(leaf);
	propagateUpdates(leaf, !splitBefore && splitAfter ? 1 : 0, rank == 0, true);
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
		root_ = oldRoot->children[oldRoot->order[0]];
		root_->parent = nullptr;
		root_->parentSlot = 0;
		nodeStore_.releaseNode(oldRoot);
	}
}

// Removes the entry at the given logical rank and repairs the tree.
// On return, leaf and rank identify the next entry, or the end position
// (rank == count) in the last leaf.
// If the tree becomes empty, leaf is nullptr and rank is zero.
void  TempConflictBTree::eraseAt(std::size_t& rank, LeafBlock*& leaf) {
	bool splitBefore=needsSplit(leaf);
	nodeStore_.eraseAt(rank,leaf);
	propagateUpdates(leaf, splitBefore && !needsSplit(leaf) ? -1 : 0,
					 rank == 0 && leaf->header.count > 0, false);
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
			// Remove the ancestors left empty. Without other leaves 
			// this reaches the root, which normalizeRoot then releases.
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
				//last remaining leaf -> empty tree
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
	unlinkLeaves(firstLeaf(child), lastLeaf(child));
	const bool splitBefore = needsSplit(node);
	if (needsSplit(child)) {
		--node->header.childrenNeedingSplit;
	}
	nodeStore_.eraseAt(rank, node);
	child->parent = nullptr;
	child->parentSlot = 0;
	// node gained a free slot and no counted child: if it needed a split,
	// it no longer does.
	assert(!needsSplit(node));
	propagateUpdates(node, splitBefore ? -1 : 0, false, false);
	// Remove the ancestors left empty, as eraseAt does for a leaf.
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
	nodeStore_.releaseNode(node);
}

void  TempConflictBTree::removeSubtree(BNodeHeader* node) {
	if (node->parent==nullptr){
		root_=nullptr;
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
	// Key blocks are reclaimed in allocation order, and a later block may
	// hold only records older than the cut: the cut must not move back.
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
		const auto slot = leaf->order[rank];
		if (leaf->cv[slot] <= reclaimCV_) {
			nodeStore_.eraseAt(rank, leaf);
			deleted = true;
		} else {
			++rank;
		}
	}

	if (splitBefore && !needsSplit(leaf)) {
		propagateUpdates(leaf, -1, false, false);
	}
	return deleted;
}

void TempConflictBTree::removeEmptyChild(
		InternalBlock* parent, std::size_t rank) {
	const auto slot = parent->order[rank];
	BNodeHeader* child = parent->children[slot];
	assert(child->count == 0);

	const bool splitBefore = needsSplit(parent);

	if (child->kind == BNodeKind::Leaf) {
		unlinkLeaf(reinterpret_cast<LeafBlock*>(child));
	}

	// The empty child no longer needs splitting. Its transition
	// was already propagated when it was cleaned.
	nodeStore_.eraseAt(rank, parent);
	nodeStore_.releaseNode(child);

	if (splitBefore && !needsSplit(parent)) {
		propagateUpdates(parent, -1, false, false);
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

	BNodeHeader* sibling;
	if (node->kind == BNodeKind::Internal) {
		sibling = &splitInternal(reinterpret_cast<InternalBlock*>(node), cv)->header;
	} else {
		assert(leaf == reinterpret_cast<LeafBlock*>(node));
		sibling = &splitLeaf(leaf, rank, found, cv)->header;
	}
	insertSibling(node, sibling, cv);
	// Propagate the sibling's conservative maximum CV.
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
	return newNode;
}

std::pair<KeyRef, Version> TempConflictBTree::minSeparator(
		const BNodeHeader* node) const {
	assert(node->count > 0);
	if (node->kind == BNodeKind::Leaf) {
		const auto* leaf = reinterpret_cast<const LeafBlock*>(node);
		const auto first = leaf->order[0];
		return {keyStore_.beginKey(leaf->ranges[first]), leaf->cv[first]};
	}
	const auto* internal = reinterpret_cast<const InternalBlock*>(node);
	const auto first = internal->order[0];
	return {internal->separators[first], internal->separatorsCV[first]};
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
		nodeStore_.insertAt(0, oldKey, node, oldCV, parent);
		nodeStore_.insertAt(1, newKey, sibling, newCV, parent);
		root_ = &parent->header;
		return;
	}

	// The parent loses a child needing a split and a free slot, so its own
	// state does not change and there is nothing to propagate.
	const bool parentSplitBefore = needsSplit(parent);
	assert(parent->freeSlotCount() > 0);
	assert(parent->header.childrenNeedingSplit > 0);
	const auto [key, keyCV] = minSeparator(sibling);
	nodeStore_.insertAt(node->parentRank() + 1, key, sibling, keyCV, parent);
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

	// The separator is the node's first key with its CV. Up the chain the
	// parent's first separator is that same key, so it is read only once.
	KeyRef separator{};
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
			parent->separators[slot] = separator;
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
