#include "test.h"
#include "pagepool.h"
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("/resolver/PagePool/AllocationOrder") {
	PagePool pool(2 * PageSize, 1024);

	const std::byte* base = pool.allocateBlock();
	ASSERT(base != nullptr);

	for (std::size_t offset = 1024; offset < 2 * PageSize; offset += 1024) {
		ASSERT_EQ(pool.allocateBlock(), base + offset);
	}

	ASSERT_EQ(pool.allocateBlock(), nullptr);
}

TEST_CASE("/resolver/PagePool/ReuseReleasedBlock") {
	PagePool pool(PageSize, 1024);

	std::byte* original = pool.allocateBlock();
	pool.releaseBlock(original);

	ASSERT_EQ(pool.allocateBlock(), original);
}

TEST_CASE("/resolver/PagePool/PageAlignment") {
	PagePool pool(3 * PageSize, PageSize);

	for (int i = 0; i < 3; ++i) {
		const auto address =
			reinterpret_cast<std::uintptr_t>(pool.allocateBlock());

		ASSERT(address != 0);
		ASSERT_EQ(address % PageSize, 0);
	}
}
#include "test.h"
#include "pagepool.h"

TEST_CASE("/resolver/PagePool/BlocksDoNotOverlap") {
	constexpr std::size_t blockSize = 1024;
	constexpr std::size_t blockCount = 3 * (PageSize / blockSize);

	PagePool pool(3 * PageSize, blockSize);
	std::byte* blocks[blockCount];

	for (std::size_t i = 0; i < blockCount; ++i) {
		blocks[i] = pool.allocateBlock();
		ASSERT(blocks[i] != nullptr);

		std::byte* address = blocks[i];

		for (std::size_t j = 0; j < blockSize; ++j) {
			address[j] = static_cast<std::byte>((i + j) % 256);
		}
	}

	for (std::size_t i = 0; i < blockCount; ++i) {
		const std::byte* address = blocks[i];

		for (std::size_t j = 0; j < blockSize; ++j) {
			ASSERT_EQ(
				address[j],
				static_cast<std::byte>((i + j) % 256)
			);
		}
	}
}
TEST_CASE("/resolver/PagePool/EmptyPool") {
	const std::size_t budgets[] = {
		0,
		1,
		PageSize - 1
	};

	for (const std::size_t budget : budgets) {
		PagePool pool(budget, 1024);

		// Repeated attempts must leave the pool empty.
		for (int attempt = 0; attempt < 3; ++attempt) {
			ASSERT_EQ(pool.allocateBlock(), nullptr);
		}
	}
}

TEST_CASE("/resolver/PagePool/PartialPageTail") {
	constexpr std::size_t blockSize = 1000;
	constexpr std::size_t pageCount = 2;
	constexpr std::size_t blocksPerPage = PageSize / blockSize;

	static_assert(blockSize >= sizeof(FreeBlock));
	static_assert(blockSize % alignof(FreeBlock) == 0);
	static_assert(PageSize % blockSize != 0);

	PagePool pool(pageCount * PageSize, blockSize);
	const std::byte* base = nullptr;

	for (std::size_t page = 0; page < pageCount; ++page) {
		for (std::size_t block = 0; block < blocksPerPage; ++block) {
			const std::byte* address = pool.allocateBlock();
			ASSERT(address != nullptr);

			if (base == nullptr) {
				base = address;
			}

			const std::size_t offset = block * blockSize;

			ASSERT_EQ(address, base + page * PageSize + offset);
			ASSERT(offset + blockSize <= PageSize);
		}
	}

	// Neither page's unused tail can supply another complete block.
	ASSERT_EQ(pool.allocateBlock(), nullptr);
}

TEST_CASE("/resolver/PagePool/MinimumBlockSize") {
	constexpr std::size_t blockSize = sizeof(FreeBlock);
	constexpr std::size_t blocksPerPage = PageSize / blockSize;

	PagePool pool(PageSize, blockSize);
	ASSERT_EQ(pool.blockSize(), blockSize);

	const std::byte* base = pool.allocateBlock();
	ASSERT(base != nullptr);

	for (std::size_t i = 1; i < blocksPerPage; ++i) {
		ASSERT_EQ(pool.allocateBlock(), base + i * blockSize);
	}

	ASSERT_EQ(pool.allocateBlock(), nullptr);
}

TEST_CASE("/resolver/PagePool/MaximumBlockSize") {
	PagePool pool(2 * PageSize, PageSize);
	ASSERT_EQ(pool.blockSize(), PageSize);

	// One block per page: the inner constructor loop runs exactly once.
	const std::byte* first = pool.allocateBlock();
	ASSERT(first != nullptr);
	ASSERT_EQ(reinterpret_cast<std::uintptr_t>(first) % PageSize, 0);
	ASSERT_EQ(pool.allocateBlock(), first + PageSize);

	ASSERT_EQ(pool.allocateBlock(), nullptr);
}

TEST_CASE("/resolver/PagePool/BudgetRoundsDownToWholePages") {
	PagePool pool(2 * PageSize + PageSize - 1, PageSize);

	const std::byte* first = pool.allocateBlock();
	ASSERT(first != nullptr);
	ASSERT_EQ(pool.allocateBlock(), first + PageSize);
	ASSERT_EQ(pool.allocateBlock(), nullptr);
}

TEST_CASE("/resolver/PagePool/BudgetAndAllocatedBytes") {
	const struct {
		std::size_t budget;
		std::size_t allocated;
	} cases[] = {
		{0, 0},
		{PageSize - 1, 0},
		{PageSize, PageSize},
		{2 * PageSize + PageSize - 1, 2 * PageSize},
		{3 * PageSize, 3 * PageSize}
	};

	for (const auto& c : cases) {
		PagePool pool(c.budget, 1024);

		ASSERT_EQ(pool.budget(), c.budget);
		ASSERT_EQ(pool.allocatedBytes(), c.allocated);

		std::size_t blocks = 0;
		while (pool.allocateBlock() != nullptr) {
			++blocks;
		}
		ASSERT_EQ(blocks * pool.blockSize(), pool.allocatedBytes());
	}
}

TEST_CASE("/resolver/PagePool/ReleaseAfterExhaustion") {
	PagePool pool(PageSize, PageSize);

	std::byte* only = pool.allocateBlock();
	ASSERT(only != nullptr);
	ASSERT_EQ(pool.allocateBlock(), nullptr);

	// Free list goes from empty back to non-empty.
	pool.releaseBlock(only);

	ASSERT_EQ(pool.allocateBlock(), only);
	ASSERT_EQ(pool.allocateBlock(), nullptr);
}

TEST_CASE("/resolver/PagePool/ReleaseIsLifo") {
	PagePool pool(2 * PageSize, 4096);

	std::byte* a = pool.allocateBlock();
	std::byte* b = pool.allocateBlock();
	std::byte* c = pool.allocateBlock();

	pool.releaseBlock(a);
	pool.releaseBlock(c);
	pool.releaseBlock(b);

	const std::byte* expected[] = {b, c, a};

	for (const std::byte* e : expected) {
		ASSERT_EQ(pool.allocateBlock(), e);
	}
}

TEST_CASE("/resolver/PagePool/ReleaseBlockFromAnotherPage") {
	PagePool pool(2 * PageSize, PageSize);

	std::byte* first = pool.allocateBlock();
	std::byte* second = pool.allocateBlock();
	ASSERT_EQ(second, first + PageSize);

	pool.releaseBlock(second);
	pool.releaseBlock(first);

	ASSERT_EQ(pool.allocateBlock(), first);
	ASSERT_EQ(pool.allocateBlock(), second);
}

// The count of free blocks starts at every block of the whole pages, goes
// down with each allocation and up with each release, and stays at zero
// when allocation fails.
TEST_CASE("/resolver/PagePool/AvailableBlocks") {
	PagePool pool(2 * PageSize + 100, 1000);
	const std::size_t total = 2 * (PageSize / 1000);
	ASSERT_EQ(pool.availableBlocks(), total);

	std::vector<std::byte*> blocks;
	for (std::size_t n = 1; n <= total; ++n) {
		blocks.push_back(pool.allocateBlock());
		ASSERT(blocks.back() != nullptr);
		ASSERT_EQ(pool.availableBlocks(), total - n);
	}
	ASSERT_EQ(pool.allocateBlock(), nullptr);
	ASSERT_EQ(pool.availableBlocks(), 0);

	for (std::size_t n = 1; n <= total; ++n) {
		pool.releaseBlock(blocks[n - 1]);
		ASSERT_EQ(pool.availableBlocks(), n);
	}
}

TEST_CASE("/resolver/PagePool/AvailableBlocksOfEmptyPool") {
	PagePool pool(PageSize - 1, 1024);
	ASSERT_EQ(pool.availableBlocks(), 0);
	ASSERT_EQ(pool.allocateBlock(), nullptr);
	ASSERT_EQ(pool.availableBlocks(), 0);
}

namespace {

// Sets RESOLVER1_PAGE_MODE for one test, or unsets it with nullptr, and
// restores the value it had before when the test ends.
class PageModeGuard {
public:
	explicit PageModeGuard(const char* mode) {
		if (const char* old = std::getenv("RESOLVER1_PAGE_MODE")) previous_ = old;
		if (mode == nullptr) {
			unsetenv("RESOLVER1_PAGE_MODE");
		} else {
			setenv("RESOLVER1_PAGE_MODE", mode, 1);
		}
	}
	~PageModeGuard() {
		if (previous_) {
			setenv("RESOLVER1_PAGE_MODE", previous_->c_str(), 1);
		} else {
			unsetenv("RESOLVER1_PAGE_MODE");
		}
	}
	PageModeGuard(const PageModeGuard&) = delete;
	PageModeGuard& operator=(const PageModeGuard&) = delete;

private:
	std::optional<std::string> previous_;
};

} // namespace

// Without RESOLVER1_PAGE_MODE, and with it set to legacy, the pool is an
// ordinary allocation of whole pages whose blocks are all available.
TEST_CASE("/resolver/PagePool/PageModeLegacyByDefaultAndByName") {
	for (const char* mode : {static_cast<const char*>(nullptr), "legacy"}) {
		PageModeGuard guard(mode);
		PagePool pool(2 * PageSize, 1024);
		ASSERT(pool.backing() == PagePool::Backing::Legacy);
		ASSERT_EQ(pool.backingBytes(), 2 * PageSize);
		ASSERT_EQ(pool.availableBlocks(), 2 * (PageSize / 1024));
		ASSERT(pool.allocateBlock() != nullptr);
	}
}

// An unknown mode is rejected when the pool is built; there is no fallback.
TEST_CASE("/resolver/PagePool/PageModeUnknownThrows") {
	PageModeGuard guard("huge");
	bool thrown = false;
	try {
		PagePool pool(PageSize, 1024);
	} catch (const std::invalid_argument&) {
		thrown = true;
	}
	ASSERT(thrown);
}

// An explicit mode with a budget below one page selects that backing, but
// there is nothing to map: the pool is empty on every platform.
TEST_CASE("/resolver/PagePool/PageModeExplicitWithoutPages") {
	const std::pair<const char*, PagePool::Backing> modes[] = {
		{"base", PagePool::Backing::BasePages},
		{"thp", PagePool::Backing::TransparentHugePages},
		{"hugetlb", PagePool::Backing::HugeTLB},
	};
	for (const auto& [mode, backing] : modes) {
		PageModeGuard guard(mode);
		for (const std::size_t budget : {std::size_t{0}, PageSize - 1}) {
			PagePool pool(budget, 1024);
			ASSERT(pool.backing() == backing);
			ASSERT_EQ(pool.backingBytes(), 0);
			ASSERT_EQ(pool.availableBlocks(), 0);
			ASSERT_EQ(pool.allocateBlock(), nullptr);
		}
	}
}

// Base pages with whole pages to map: on Linux the pool is mapped aligned to
// 2 MiB, rounded up to whole 2 MiB, and all its blocks are available;
// elsewhere an explicit backing is rejected.
TEST_CASE("/resolver/PagePool/PageModeBaseWithPages") {
	PageModeGuard guard("base");
#ifdef __linux__
	PagePool pool(2 * PageSize, 1024);
	constexpr std::size_t HugeBytes = 2 * 1024 * 1024;
	ASSERT(pool.backing() == PagePool::Backing::BasePages);
	ASSERT_EQ(reinterpret_cast<std::uintptr_t>(pool.backingAddress()) % HugeBytes, 0);
	ASSERT_EQ(pool.backingBytes(), (2 * PageSize + HugeBytes - 1) / HugeBytes * HugeBytes);
	ASSERT_EQ(pool.availableBlocks(), 2 * (PageSize / 1024));
	ASSERT(pool.allocateBlock() != nullptr);
#else
	bool thrown = false;
	try {
		PagePool pool(2 * PageSize, 1024);
	} catch (const std::runtime_error&) {
		thrown = true;
	}
	ASSERT(thrown);
#endif
}
