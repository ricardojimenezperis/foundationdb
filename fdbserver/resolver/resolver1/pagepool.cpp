// Page pool
//
#include "pagepool.h"

#include <cassert>
#include <new>

PagePool::PagePool(std::size_t budget, std::size_t blockSize)
	: budget_(budget), blockSize_(blockSize) {
	assert(blockSize >= sizeof(FreeBlock) &&
		   blockSize <= PageSize &&
		   blockSize % alignof(FreeBlock) == 0);
	numPages_ = budget / PageSize;
	blocksPerPage_ = PageSize / blockSize;

	freeBlocks_ = nullptr;
	pages_ = new Page[numPages_];

	for (std::size_t p = numPages_; p > 0;) {
		--p;
		for (std::size_t b = blocksPerPage_; b > 0;) {
			--b;
			releaseBlock(pages_[p].data + b * blockSize);
		}
	}
}

PagePool::~PagePool() {
	delete[] pages_;
}

std::byte* PagePool::allocateBlock() {
	if (freeBlocks_ == nullptr) {
		return nullptr;
	}

	FreeBlock* block = freeBlocks_;
	freeBlocks_ = block->next;
	--availableBlocks_;

	return reinterpret_cast<std::byte*>(block);
}

void PagePool::releaseBlock(void* block) {
	freeBlocks_ = ::new (block) FreeBlock{freeBlocks_};
	++availableBlocks_;
}
