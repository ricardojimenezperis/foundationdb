#include "test.h"
#include "pagepool.h"
#include <cstdint>
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
