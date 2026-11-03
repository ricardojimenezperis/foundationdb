// Page pool
//
// Manages aligned pages divided into fixed-size blocks under a fixed memory budget.
// Each block can hold either a B+ tree node or a block of key records.
// Both uses share the same free-block list, so released blocks can be reused
// for either purpose.
//
// Provides block allocation and release, and counts the free blocks, so
// that callers can reclaim history until enough blocks are available.
// History reclamation is handled by the caller.
#ifndef RESOLVER1_PAGEPOOL_H
#define RESOLVER1_PAGEPOOL_H

#include <cstddef>
#include <cstdint>
#include "common.h"

struct FreeBlock {
	FreeBlock* next;
};

struct alignas(PageSize) Page {
	std::byte data[PageSize];
};

static_assert(sizeof(Page) == PageSize);

class PagePool {
public:
	enum class Backing { Legacy, BasePages, TransparentHugePages, HugeTLB };
	// RESOLVER1_PAGE_MODE: legacy (default), base, thp, or hugetlb (2 MiB).
	// Explicit modes fail on allocation/advice errors; there is no fallback.
	PagePool(std::size_t budget, std::size_t blockSize);
	~PagePool();

	PagePool(const PagePool&) = delete;
	PagePool& operator=(const PagePool&) = delete;

	std::byte* allocateBlock();
	void releaseBlock(void* block);

	std::size_t blockSize() const { return blockSize_; }
	std::size_t budget() const { return budget_; }
	std::size_t allocatedBytes() const { return numPages_ * PageSize; }
	// Free blocks, whichever store released them.
	std::size_t availableBlocks() const { return availableBlocks_; }
	Backing backing() const { return backing_; }
	const void* backingAddress() const { return pages_; }
	std::size_t backingBytes() const { return mappedBytes_ == 0 ? allocatedBytes() : mappedBytes_; }

private:
	std::size_t budget_;
	std::size_t blockSize_;

	std::size_t blocksPerPage_;
	std::size_t numPages_ = 0;

	FreeBlock* freeBlocks_ = nullptr;
	std::size_t availableBlocks_ = 0;
	Page* pages_ = nullptr;
	Backing backing_ = Backing::Legacy;
	std::size_t mappedBytes_ = 0;
};

#endif
