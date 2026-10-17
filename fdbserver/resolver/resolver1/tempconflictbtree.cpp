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

	const int MaxWriteBlocks=35;
	reclaimUntilAvail(MaxWriteBlocks);
	LeafBlock* leaf=nullptr;
	std::size_t rank=0;
	bool found=findKey(write.first, leaf, rank);
	if (leaf==nullptr){
		std::optional<RangeRef> range= std::nullopt;
		range=keyStore_.appendRange(write.first,write.second,cv);
		leaf=nodeStore_.allocateLeaf(cv);
		nodeStore_.insertAt(std::size_t{0},*range,cv,leaf);
		return;
	} 
	if (!found){ 
		if (rank>0){
			--rank;
		} else {
			if (leaf->left!=nullptr){
				leaf=leaf->left;
				rank=leaf->header.count-1;
			}
		}
	} 
	if (!found && leaf){
		leaf->ranges[rank]=
	}
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