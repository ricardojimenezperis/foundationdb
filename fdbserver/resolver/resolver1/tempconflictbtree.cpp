#include "tempconflictbtree.h"
#include "bnodestore.h"
#include <numeric>
#include <stdexcept>

TempConflictBTree::TempConflictBTree(std::size_t budget)
	: pool_(budget, BlockSize),
	  keyStore_(pool_),
	  nodeStore_(pool_, keyStore_) {
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
	// Blocks one write may take: its range and, when it trims stored ranges,
	// a left and a right exterior, each two keys no longer than the longest
	// key written so far, plus two nodes (a split and a new root). The
	// reserve is never below 35 blocks, which covers keys of up to 11,132
	// bytes with 2 KiB blocks; a smaller pool only needs to hold the write.
	// Reclaiming happens only here, before the write reads or stores any
	// key, so none of its stores can fail.
	constexpr std::size_t MinWriteBlocks = 35;
	const std::size_t keyLength = std::max({maxKeyLength_, static_cast<std::size_t>(write.first.size()),
											static_cast<std::size_t>(write.second.size())});
	const std::size_t payload = keyStore_.blockSize() - sizeof(KeyBlockHeader);
	const std::size_t recordBlocks = (2 * keyLength + payload - 1) / payload;
	const std::size_t writeBlocks = 3 * recordBlocks + 2;
	if (!reclaimUntilAvail(std::max(MinWriteBlocks, writeBlocks)) &&
		pool_.availableBlocks() < writeBlocks) {
		throw std::length_error("TempConflictBTree: the pool cannot hold one write");
	}
	maxKeyLength_ = keyLength;
	LeafBlock* leaf = nullptr;
	std::size_t rank = 0;
	BNodeHeader* nodeNeedsSplit = nullptr;
	const bool found = findKey(write.first, leaf, rank, nodeNeedsSplit);
	if (nodeNeedsSplit != nullptr) {
		split(nodeNeedsSplit, leaf, rank, found, cv);
	}
	auto range = keyStore_.appendRange(write.first, write.second, cv);
	assert(range.has_value());
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
		assert(left.has_value());
		previousLeaf->ranges[slot] = *left;
		if (keyStore_.compareEnd(write.second, oldRef) < 0) {
			// The write lies inside that range: its right exterior follows the
			// write, in the same leaf, even when the range is in the leaf
			// before. Live entries are disjoint, so the leaf's own entries
			// begin after the range ends.
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
	assert(right.has_value());
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
				// The last leaf was removed.
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
	// No entry is added while cleanups are deferred: a marked child stays
	// without live entries until cleanPending removes it.
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

void TempConflictBTree::removeIfEmpty(BNodeHeader* node) {
	if (node->count > 0) {
		return;
	}
	InternalBlock* parent = node->parent;
	if (parent == nullptr) {
		if (node->kind == BNodeKind::Internal) {
			root_ = nullptr;
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

	// The parent loses a child needing a split and a free slot, so its own
	// state does not change and there is nothing to propagate.
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

	// The separator is the node's first key with its CV. Up the chain the
	// parent's first separator is that same key, so it is read only once.
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
		// A leaf released since it was queued, by an earlier cleanup of the
		// queue, has its generation cleared. Nothing is allocated before the
		// queue is emptied, so its block is not reused meanwhile.
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

	// Consuming states separate a prefetch hint from a later task visit.
	// Local work can continue directly; scans yield at leaf boundaries.
	enum class Step : std::uint8_t {
		Empty,             // Unused slot.
		InspectNode,       // Node prefetched: prepare internal/leaf search.
		CompareKeys,       // Separator and midpoint read prefetched.
		CompareLeafKey,    // Entry key and first read begin prefetched.
		FindPrev,          // A leaf to the left prefetched: find a live entry.
		CheckPrev,         // Its end and the current read begin prefetched.
		StartScan,         // Initialize the unresolved-read list at the saved scan start.
		ScanLeaf,          // Scan pending reads against the current leaf.
		Done               // Release the slot.
	};
	struct Task {
		NodeRef node{nullptr};
		uint32_t readBegin = 0; // Assigned read slice, end exclusive.
		uint32_t readEnd = 0;
		int32_t lo = 0;        // Internal node or leaf search bounds.
		int32_t hi = 0;
		union {
			uint32_t readLo = 0;  // Internal partition search lower bound.
			uint32_t readReachedEnd; // Scan: earlier reads' begins were reached.
		};
		uint32_t readHi = 0;
		uint32_t readIndex = 0; // Read being scanned.
		uint32_t readHead = 0;  // First pending read; readEnd is the sentinel.
		uint32_t readPrev = 0;  // Predecessor in the current scan pass.
		uint32_t rank = 0;      // Leaf entry being scanned.
		LeafBlock* prevLeaf = nullptr;
		uint32_t prevRank = 0;
		Step step = Step::Empty;
	};
	static_assert(G > 0);
	Task tasks[G]{};
	uint16_t order[G]{};
	uint32_t active = 0;
	bool completedTasks = false;
	FreeSlots<G> freeSlots;
	std::vector<Task> pending; // Lowest priority first; back() resumes next.
	// Read slices are disjoint once they reach their leaf tasks. Each task
	// owns the links in its slice; tasks kept in pending retain their cursors.
	std::vector<uint32_t> nextRead;
	struct NewTask {
		uint32_t parent; // Position in this round's order, not a physical slot.
		Task task;
	};
	std::vector<NewTask> newTasks;
	newTasks.reserve(G); // At most one right sibling per active task.

	conflicts.assign(reads.size(), 0);
	readBegins.clear();
	writeBegins.clear();
	readBegins.reserve(points.size() / 2);
	writeBegins.reserve(points.size() / 2);
	auto buildBeginIndexes = [&]() {
		const uint32_t pointsSize = points.size();
		for (uint32_t p = 0; p < pointsSize; ++p) {
			const KeyInfo& point = points[p];
			if (!point.begin) continue;
			const uint32_t t = point.transaction;
			TransactionInfo& info = *transactionInfo[t];
			const auto& ranges =
				point.write ? info.writeRanges : info.readRanges;
			const uint32_t r =
				(reinterpret_cast<const char*>(point.pIndex) -
				 reinterpret_cast<const char*>(ranges.begin())) /
				sizeof(std::pair<int, int>);
			if (point.write) {
				writeBegins.push_back({r, t});
				continue;
			}
			if (info.tooOld) continue;
			const uint32_t rangeIndex = offsets[t] + r;
			const ReadConflictRange& read = reads[rangeIndex];
			if (read.version < reclaimCV_) {
				info.tooOld = true;
				transactionConflictStatus[t] = true;
				++tooOldWithinWindow;
				continue;
			}
			if (transactionConflictStatus[t] &&
				read.conflictingKeyRange == nullptr) continue;
			if (read.begin.compare(read.end) >= 0) continue;
			readBegins.push_back({rangeIndex, t});
		}
		uint32_t kept = 0;
		const uint32_t count = readBegins.size();
		for (uint32_t i = 0; i < count; ++i) {
			const RangeEntry entry = readBegins[i];
			if (transactionInfo[entry.transaction]->tooOld) continue;
			readBegins[kept++] = entry;
		}
		readBegins.resize(kept);
	};

	auto markConflict = [&](uint32_t readIndex) {
		const RangeEntry& entry = readBegins[readIndex];
		conflicts[entry.rangeIndex] = 1;
		transactionConflictStatus[entry.transaction] = true;
	};

	auto readAt = [&](uint32_t rank) -> const ReadConflictRange& {
		return reads[readBegins[rank].rangeIndex];
	};

	auto readStillNeeded = [&](uint32_t rank) -> bool {
		const RangeEntry& entry = readBegins[rank];
		if (transactionInfo[entry.transaction]->tooOld ||
			conflicts[entry.rangeIndex]) return false;
		return !transactionConflictStatus[entry.transaction] ||
			reads[entry.rangeIndex].conflictingKeyRange != nullptr;
	};

	// Unlink a resolved read in O(1), preserving the order of the remaining
	// reads. readBegin/readEnd and readBegins stay unchanged for routing.
	auto removeScanRead = [&](Task& task) {
		assert(task.readIndex < task.readEnd);
		const uint32_t next = nextRead[task.readIndex];
		assert(next <= task.readEnd);
		if (task.readPrev == task.readEnd) {
			task.readHead = next;
		} else {
			nextRead[task.readPrev] = next;
		}
		task.readIndex = next;
	};

	// Compact a leaf before its first saved rank in this batch. With a
	// fixed cutoff and no writes, later visits preserve every saved rank.
	// Empty leaves stay linked until all tasks have finished.
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
		task.readBegin = task.readLo = 0;
		task.readEnd = task.readHi = readBegins.size();
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

	// Follow block links only when the requested prefetch crosses a block.
	// Check validity before dereferencing a potentially reclaimed header.
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

	// Requires live referenced bytes throughout the access. Up to two
	// cache lines can be hinted without reading a same-block header; only
	// the linked-block path needs to inspect headers.
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

	// Continuations: each issues the prefetch that its step consumes in a
	// later visit, and sets that step.
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
			// Refresh before prefetching: the old key block may be reused.
			// Deferred cleanup preserves every task's ranks during the batch.
			refreshSeparatorDeferred(node, mid);
		}
		if (node->separatorsCV[slot] != EmptyChildCV) {
			prefetchSepKey(task.node, mid);
		}
		prefetchReadKey(std::midpoint(task.readLo, task.readHi));
		task.step = Step::CompareKeys;
	};

	auto toCompareLeafKey = [&](Task& task) {
		const auto* leaf = task.node.leaf;
		const int32_t mid = std::midpoint(task.lo, task.hi);
		task.rank = static_cast<uint32_t>(mid);
		prefetchRangeBegin(leaf->ranges[leaf->order[mid]]);
		prefetchReadKey(task.readBegin);
		task.step = Step::CompareLeafKey;
	};

	auto toCheckPrev = [&](Task& task) {
		if (task.readIndex == task.readBegin) {
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

	// Suspension may outlast cache residency. Reissue hints for the
	// resumed state without assuming its earlier data is still cached.
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

	// Gives the task the reads [readBegin, readEnd) for the children of its
	// internal node delimited by separators [lo, hi), that is, children
	// lo - 1 to hi - 1. Descends when only one child remains.
	auto assignChildren = [&](Task& task, int32_t lo, int32_t hi,
						  uint32_t readBegin, uint32_t readEnd) {
		task.readBegin = task.readLo = readBegin;
		task.readEnd = task.readHi = readEnd;
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

	auto inspectNode = [&](Task& task) {
		if (task.node.header->kind == BNodeKind::Leaf) {
			cleanReadLeaf(task.node.leaf);
		}
		task.hi = task.node.header->count;

		if (task.node.header->kind == BNodeKind::Leaf) {
			task.lo = 0;
			task.readIndex = task.readBegin;

			if (task.hi == 0) {
				task.rank = 0;
				task.prevLeaf = task.node.leaf;
				task.prevRank = 0;
				findPrev(task);
			} else {
				toCompareLeafKey(task);
			}
			return;
		}

		if (task.hi == 0) {
			task.step = Step::Empty;
			completedTasks = true;
			return;
		}

		assignChildren(task, 1, task.hi,
					   task.readBegin, task.readEnd);
	};

	auto compareKeys = [&](Task& task, uint32_t position) {
		// Find the first read whose begin is >= separator mid.
		const int32_t mid = std::midpoint(task.lo, task.hi);
		const uint32_t midRead = std::midpoint(task.readLo, task.readHi);
		const uint32_t slot = task.node.internal->order[mid];
		const RangeRef& separator = task.node.internal->separators[slot];
		if (task.node.internal->separatorsCV[slot] == EmptyChildCV) {
			// Empty markers are not comparable keys, even when a live
			// neighbour was copied. Route left so forward scans retain all
			// possible overlaps, including beyond an entirely empty subtree.
			task.readLo = task.readHi;
		} else if (keyStore_.compareBegin(readAt(midRead).begin, separator) < 0) {
			task.readLo = midRead + 1;
		} else {
			task.readHi = midRead;
		}
		if (task.readLo < task.readHi) {
			toCompareKeys(task);
			return;
		}
		const uint32_t split = task.readLo;
		if (split == task.readEnd) {
			assignChildren(task, task.lo, mid, task.readBegin, task.readEnd);
		} else if (split == task.readBegin) {
			assignChildren(task, mid + 1, task.hi, task.readBegin, task.readEnd);
		} else {
			// The sibling prefetches its first access now; endRound admits it.
			Task right = task;
			assignChildren(right, mid + 1, task.hi, split, task.readEnd);
			assignChildren(task, task.lo, mid, task.readBegin, split);
			newTasks.push_back({position, right});
		}
	};

	auto compareLeafKey = [&](Task& task) {
		const int32_t mid = static_cast<int32_t>(task.rank);
		const auto* leaf = task.node.leaf;
		const auto slot = leaf->order[mid];
		const int cmp = keyStore_.compareBegin(
			readAt(task.readBegin).begin, leaf->ranges[slot]);

		if (cmp == 0) {
			task.rank = mid;
			task.readIndex = task.readBegin;
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
		task.readIndex = task.readBegin;
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

		++task.readIndex;
		while (task.readIndex < task.readEnd &&
			   !readStillNeeded(task.readIndex)) {
			++task.readIndex;
		}

		if (task.readIndex < task.readEnd) {
			toCheckPrev(task);
		} else {
			task.step = Step::StartScan;
		}
	};

	// A scan yields only at a leaf boundary or when the task finishes.
	// Key comparisons run directly, without per-pair scheduling/prefetches.
	auto scanLeaf = [&](Task& task) {
		auto* leaf = task.node.leaf;
		cleanReadLeaf(leaf);

		while (task.rank < leaf->header.count) {
			const auto slot = leaf->order[task.rank];
			const auto& write = leaf->ranges[slot];
			while (task.readIndex < task.readEnd) {
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
					// Ordered disjoint writes have increasing ends, so a reached
					// read begin stays below every later write end.
					task.readReachedEnd = task.readIndex + 1;
				}

				if (keyStore_.compareBegin(read.end, write) <= 0) {
					removeScanRead(task);
				} else if (leaf->cv[slot] > read.version) {
					markConflict(task.readIndex);
					removeScanRead(task);
				} else {
					// Keep this read pending for later writes.
					task.readPrev = task.readIndex;
					task.readIndex = nextRead[task.readIndex];
				}
			}

			if (task.readHead == task.readEnd) {
				task.step = Step::Done;
				completedTasks = true;
				return;
			}
			++task.rank;
			task.readIndex = task.readHead;
			task.readPrev = task.readEnd;
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
		// Partition and scan phases are exclusive, so the union can reuse
		// readLo for the reached prefix of sorted read begins.
		task.readReachedEnd = task.readBegin;
		task.readHead = task.readBegin;
		task.readIndex = task.readHead;
		task.readPrev = task.readEnd;
		while (task.readIndex < task.readEnd &&
			   !readStillNeeded(task.readIndex)) {
			removeScanRead(task);
		}

		if (task.readIndex == task.readEnd) {
			task.step = Step::Done;
			completedTasks = true;
			return;
		}

		scanLeaf(task);
	};

	// New siblings wait until the next round; active order is fixed here.
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

	auto endRound = [&]() {
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
	if (root_ == nullptr || readBegins.empty()) return;
	nextRead.resize(readBegins.size());
	std::iota(nextRead.begin(), nextRead.end(), uint32_t{1});
	initializeRootTask();
	prefetchBlock(tasks[0].node);
	while (active != 0) {
		runRound();
		endRound();
	}
	cleanPending();
}

// 16 is the default task capacity of the FDB integration and the benchmark;
// the tests compare it with 32. Other capacities need their own instantiation.
template void TempConflictBTree::checkReads<16>(
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
