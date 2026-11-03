// Page pool
//
#include "pagepool.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>

#ifdef __linux__
#include <sys/mman.h>
#endif

PagePool::PagePool(std::size_t budget, std::size_t blockSize)
	: budget_(budget), blockSize_(blockSize) {
	assert(blockSize >= sizeof(FreeBlock) &&
		   blockSize <= PageSize &&
		   blockSize % alignof(FreeBlock) == 0);
	numPages_ = budget / PageSize;
	blocksPerPage_ = PageSize / blockSize;

	freeBlocks_ = nullptr;
	const char* mode = std::getenv("RESOLVER1_PAGE_MODE");
	if (mode == nullptr || std::strcmp(mode, "legacy") == 0) {
		backing_ = Backing::Legacy;
	} else if (std::strcmp(mode, "base") == 0) {
		backing_ = Backing::BasePages;
	} else if (std::strcmp(mode, "thp") == 0) {
		backing_ = Backing::TransparentHugePages;
	} else if (std::strcmp(mode, "hugetlb") == 0) {
		backing_ = Backing::HugeTLB;
	} else {
		throw std::invalid_argument("PagePool: unknown RESOLVER1_PAGE_MODE");
	}
	if (backing_ == Backing::Legacy || numPages_ == 0) {
		pages_ = new Page[numPages_];
	} else {
#ifdef __linux__
		constexpr std::size_t HugeBytes = 2 * 1024 * 1024;
		static_assert(HugeBytes % PageSize == 0);
		const std::size_t usedBytes = numPages_ * PageSize;
		if (usedBytes > std::numeric_limits<std::size_t>::max() - 2 * HugeBytes) {
			throw std::bad_alloc();
		}
		mappedBytes_ = (usedBytes + HugeBytes - 1) / HugeBytes * HugeBytes;
		if (backing_ == Backing::HugeTLB) {
			// Linux MAP_HUGE_SHIFT is 26; 21 selects 2 MiB pages explicitly.
			void* mapping = mmap(nullptr, mappedBytes_, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (21 << 26), -1, 0);
			if (mapping == MAP_FAILED) {
				throw std::system_error(errno, std::generic_category(), "PagePool: mmap HugeTLB 2 MiB");
			}
			pages_ = static_cast<Page*>(mapping);
		} else {
			const std::size_t reserveBytes = mappedBytes_ + HugeBytes;
			void* mapping = mmap(nullptr, reserveBytes, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (mapping == MAP_FAILED) {
				throw std::system_error(errno, std::generic_category(), "PagePool: mmap aligned pool");
			}
			const auto start = reinterpret_cast<std::uintptr_t>(mapping);
			const auto aligned = (start + HugeBytes - 1) / HugeBytes * HugeBytes;
			const std::size_t prefix = aligned - start;
			const std::size_t suffix = reserveBytes - prefix - mappedBytes_;
			pages_ = reinterpret_cast<Page*>(aligned);
			if (prefix != 0) munmap(mapping, prefix);
			if (suffix != 0) munmap(reinterpret_cast<void*>(aligned + mappedBytes_), suffix);
			const int advice = backing_ == Backing::TransparentHugePages ? MADV_HUGEPAGE : MADV_NOHUGEPAGE;
			if (madvise(pages_, mappedBytes_, advice) != 0) {
				const int error = errno;
				munmap(pages_, mappedBytes_);
				pages_ = nullptr;
				throw std::system_error(error, std::generic_category(), "PagePool: hugepage advice");
			}
		}
		// Begin Page object lifetimes without zeroing the byte arrays.
		for (std::size_t p = 0; p < numPages_; ++p) ::new (pages_ + p) Page;
#else
		throw std::runtime_error("PagePool: explicit page backing requires Linux");
#endif
	}

	for (std::size_t p = numPages_; p > 0;) {
		--p;
		for (std::size_t b = blocksPerPage_; b > 0;) {
			--b;
			releaseBlock(pages_[p].data + b * blockSize);
		}
	}
}

PagePool::~PagePool() {
	if (mappedBytes_ != 0) {
#ifdef __linux__
		munmap(pages_, mappedBytes_);
#endif
	} else {
		delete[] pages_;
	}
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
