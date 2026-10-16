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