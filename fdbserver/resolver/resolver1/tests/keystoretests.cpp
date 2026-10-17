#include "test.h"
#include "keystore.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

// With one block per page, a key of this length fills a whole key block.
constexpr std::size_t FullBlockKeyLength = PageSize - sizeof(KeyBlockHeader);

const std::byte ShortKey[] = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

std::optional<KeyRef> appendFullBlockKey(KeyStore& store, Version cv) {
	const std::vector<std::byte> key(FullBlockKeyLength, std::byte{0x11});

	return store.appendKey(
		key.data(), static_cast<std::uint16_t>(key.size()), cv);
}

bool storedBytesEqual(const KeyRef& ref,
					  const std::byte* expected,
					  std::size_t length) {
	const std::byte* stored =
		reinterpret_cast<const std::byte*>(ref.firstBlock) + ref.keyOffset;

	return std::memcmp(stored, expected, length) == 0;
}

} // namespace

// The only key block is filled, the pool is exhausted, and reclamation
// empties the block list. The next key must start a new list.
TEST_CASE("/resolver/KeyStore/AppendAfterReclaimingAllBlocks") {
	PagePool pool(PageSize, PageSize);
	KeyStore store(pool);

	ASSERT(appendFullBlockKey(store, 10).has_value());
	ASSERT(!store.appendKey(ShortKey, sizeof(ShortKey), 11).has_value());

	ASSERT(store.reclaimOldest() == Version{10});

	const std::optional<KeyRef> ref = store.appendKey(ShortKey, sizeof(ShortKey), 12);

	ASSERT(ref.has_value());
	ASSERT_EQ(ref->keyOffset, sizeof(KeyBlockHeader));
	ASSERT_EQ(ref->keyLength, sizeof(ShortKey));
	ASSERT(storedBytesEqual(*ref, ShortKey, sizeof(ShortKey)));

	const KeyBlockHeader* header = ref->firstBlock;

	ASSERT_EQ(header->generation, ref->blockGen);
	ASSERT(header->nextBlock == nullptr);

	// The new block must be reachable from the list, so it is reclaimed
	// and returned to the pool.
	ASSERT(store.reclaimOldest() == Version{12});
	ASSERT(!store.reclaimOldest().has_value());
	ASSERT(pool.allocateBlock() != nullptr);
}

// Every key block is full and the pool is exhausted, but only the oldest
// block is reclaimed. The next key must be linked after the last full block.
TEST_CASE("/resolver/KeyStore/AppendAfterReclaimingOldestBlock") {
	PagePool pool(2 * PageSize, PageSize);
	KeyStore store(pool);

	ASSERT(appendFullBlockKey(store, 10).has_value());
	ASSERT(appendFullBlockKey(store, 20).has_value());
	ASSERT(!store.appendKey(ShortKey, sizeof(ShortKey), 21).has_value());

	ASSERT(store.reclaimOldest() == Version{10});

	const std::optional<KeyRef> ref = store.appendKey(ShortKey, sizeof(ShortKey), 30);

	ASSERT(ref.has_value());
	ASSERT_EQ(ref->keyOffset, sizeof(KeyBlockHeader));
	ASSERT(storedBytesEqual(*ref, ShortKey, sizeof(ShortKey)));

	// Reclamation must follow the list from the full block to the new one.
	ASSERT(store.reclaimOldest() == Version{20});
	ASSERT(storedBytesEqual(*ref, ShortKey, sizeof(ShortKey)));
	ASSERT(store.reclaimOldest() == Version{30});
	ASSERT(!store.reclaimOldest().has_value());

	// Both blocks are back in the pool.
	ASSERT(pool.allocateBlock() != nullptr);
	ASSERT(pool.allocateBlock() != nullptr);
}

namespace {

std::vector<std::byte> makeKey(std::size_t length, unsigned seed) {
	std::vector<std::byte> key(length);

	for (std::size_t i = 0; i < length; ++i) {
		key[i] = static_cast<std::byte>((seed + i) % 251);
	}

	return key;
}

std::optional<KeyRef> append(KeyStore& store,
							 const std::vector<std::byte>& key,
							 Version cv) {
	return store.appendKey(
		key.data(), static_cast<std::uint16_t>(key.size()), cv);
}

// Reads a key through the public interface only, following the block chain.
// Optionally reports how many blocks the key spans.
std::vector<std::byte> readKey(const KeyStore& store,
							   const KeyRef& ref,
							   std::size_t* blocksSpanned = nullptr) {
	std::vector<std::byte> bytes;

	// The empty key has no storage.
	if (ref.keyLength == 0) {
		if (blocksSpanned != nullptr) {
			*blocksSpanned = 0;
		}
		return bytes;
	}

	const KeyBlockHeader* block = store.keyBlock(ref);
	ASSERT(block != nullptr);
	const std::byte* data = store.keyStart(ref);
	std::size_t available = store.blockSize() - ref.keyOffset;
	std::size_t remaining = ref.keyLength;
	std::size_t blocks = 1;

	for (;;) {
		const std::size_t take = std::min(remaining, available);

		bytes.insert(bytes.end(), data, data + take);
		remaining -= take;

		if (remaining == 0) {
			break;
		}

		block = block->nextBlock;
		ASSERT(block != nullptr);
		data = reinterpret_cast<const std::byte*>(block) + sizeof(KeyBlockHeader);
		available = store.blockSize() - sizeof(KeyBlockHeader);
		++blocks;
	}

	if (blocksSpanned != nullptr) {
		*blocksSpanned = blocks;
	}

	return bytes;
}

// Takes every free block from the pool, leaving `keep` of them free.
std::vector<std::byte*> drainPool(PagePool& pool, std::size_t keep) {
	std::vector<std::byte*> taken;

	for (std::byte* block = pool.allocateBlock(); block != nullptr;
		 block = pool.allocateBlock()) {
		taken.push_back(block);
	}

	for (std::size_t i = 0; i < keep; ++i) {
		pool.releaseBlock(taken.back());
		taken.pop_back();
	}

	return taken;
}

std::size_t freeBlockCount(PagePool& pool) {
	const std::vector<std::byte*> taken = drainPool(pool, 0);

	for (std::byte* block : taken) {
		pool.releaseBlock(block);
	}

	return taken.size();
}

// 64-byte blocks leave 40 payload bytes after the 24-byte header.
constexpr std::size_t SmallBlock = 64;
constexpr std::size_t SmallPayload = SmallBlock - sizeof(KeyBlockHeader);

} // namespace

TEST_CASE("/resolver/KeyStore/DestructorReturnsAllBlocks") {
	PagePool pool(PageSize, SmallBlock);
	const std::size_t freeBefore = freeBlockCount(pool);

	{
		KeyStore store(pool);

		ASSERT(append(store, makeKey(15, 1), 1).has_value());
		ASSERT(append(store, makeKey(SmallPayload * 3, 2), 2).has_value());
		ASSERT(append(store, makeKey(SmallPayload * 2 + 7, 3), 3).has_value());
		ASSERT(freeBlockCount(pool) < freeBefore);
	}

	ASSERT_EQ(freeBlockCount(pool), freeBefore);
}

// A reclaimed block reused for a new key keeps its address but not its
// identity: references to the old key must not become valid again.
TEST_CASE("/resolver/KeyStore/ReusedBlockDoesNotReviveOldReferences") {
	PagePool pool(PageSize, PageSize);
	KeyStore store(pool);

	const auto oldKey = makeKey(10, 1);
	const std::optional<KeyRef> oldRef = append(store, oldKey, 1);
	ASSERT(oldRef.has_value());

	ASSERT(store.reclaimOldest() == Version{1});

	const auto newKey = makeKey(10, 2);
	const std::optional<KeyRef> newRef = append(store, newKey, 2);
	ASSERT(newRef.has_value());

	ASSERT(newRef->firstBlock == oldRef->firstBlock);
	ASSERT_EQ(newRef->keyOffset, oldRef->keyOffset);
	ASSERT(newRef->blockGen > oldRef->blockGen);

	ASSERT(!store.isValid(*oldRef));
	ASSERT(store.isValid(*newRef));
	ASSERT(readKey(store, *newRef) == newKey);
}

// A failed multi-block append must leave earlier keys and the open
// block's state as they were.
TEST_CASE("/resolver/KeyStore/FailedAppendKeepsExistingKeys") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const auto existing = makeKey(10, 1);
	const std::optional<KeyRef> ref = append(store, existing, 5);
	ASSERT(ref.has_value());

	const std::vector<std::byte*> taken = drainPool(pool, 1);

	// Needs the open block and two more; only one is available.
	ASSERT(!append(store, makeKey(SmallPayload * 3, 2), 9).has_value());
	ASSERT_EQ(freeBlockCount(pool), 1);

	ASSERT(store.isValid(*ref));
	ASSERT(readKey(store, *ref) == existing);
	ASSERT_EQ(store.keyBlock(*ref)->maxCV, Version{5});
	ASSERT(store.oldestCV() == Version{5});

	const auto next = makeKey(4, 3);
	const std::optional<KeyRef> nextRef = append(store, next, 6);
	ASSERT(nextRef.has_value());
	ASSERT(nextRef->firstBlock == ref->firstBlock);
	ASSERT_EQ(nextRef->keyOffset, sizeof(KeyBlockHeader) + existing.size());
	ASSERT(readKey(store, *nextRef) == next);

	for (std::byte* block : taken) {
		pool.releaseBlock(block);
	}
}

// Commit versions may arrive out of order; maxCV keeps the maximum.
TEST_CASE("/resolver/KeyStore/OutOfOrderCV") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const std::optional<KeyRef> a = append(store, makeKey(5, 1), 20);
	const std::optional<KeyRef> b = append(store, makeKey(5, 2), 10);
	ASSERT(a.has_value() && b.has_value());
	ASSERT(a->firstBlock == b->firstBlock);

	ASSERT_EQ(store.keyBlock(*b)->maxCV, Version{20});
	ASSERT(store.oldestCV() == Version{20});
	ASSERT(store.reclaimOldest() == Version{20});
}

// Blocks with the same commit version are still reclaimed one at a time.
TEST_CASE("/resolver/KeyStore/EqualCVBlocksReclaimedIndividually") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// a fills the first block exactly; b goes to the second one.
	const std::optional<KeyRef> a = append(store, makeKey(SmallPayload, 1), 7);
	const std::optional<KeyRef> b = append(store, makeKey(10, 2), 7);
	ASSERT(a.has_value() && b.has_value());
	ASSERT(!(a->firstBlock == b->firstBlock));

	ASSERT(store.reclaimOldest() == Version{7});
	ASSERT(!store.isValid(*a));
	ASSERT(store.isValid(*b));
	ASSERT(store.oldestCV() == Version{7});

	ASSERT(store.reclaimOldest() == Version{7});
	ASSERT(!store.isValid(*b));
}

TEST_CASE("/resolver/KeyStore/EmptyPool") {
	PagePool pool(0, SmallBlock);
	KeyStore store(pool);

	ASSERT(!store.oldestCV().has_value());
	ASSERT(!store.reclaimOldest().has_value());
	ASSERT(!append(store, makeKey(4, 1), 5).has_value());

	const std::optional<KeyRef> empty = store.appendKey(nullptr, 0, 6);
	ASSERT(empty.has_value());
	ASSERT(store.isValid(*empty));
}

TEST_CASE("/resolver/KeyStore/BlockSize") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	ASSERT_EQ(store.blockSize(), SmallBlock);
}

// The empty key is a valid FDB key: it is accepted without using storage.
TEST_CASE("/resolver/KeyStore/EmptyKey") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const std::optional<KeyRef> empty = store.appendKey(nullptr, 0, 5);

	ASSERT(empty.has_value());
	ASSERT_EQ(empty->keyLength, 0);
	ASSERT(store.keyStart(*empty) == nullptr);
	ASSERT(store.keyBlock(*empty) == nullptr);
	ASSERT(store.isValid(*empty));
	ASSERT(readKey(store, *empty).empty());

	// No bytes consumed and no version recorded.
	ASSERT(!store.oldestCV().has_value());

	const auto next = makeKey(4, 1);
	const std::optional<KeyRef> ref = append(store, next, 6);
	ASSERT(ref.has_value());
	ASSERT_EQ(ref->keyOffset, sizeof(KeyBlockHeader));
}

TEST_CASE("/resolver/KeyStore/EmptyKeyWithoutFreeBlocks") {
	PagePool pool(PageSize, PageSize);
	KeyStore store(pool);

	// The only block is full and the pool is exhausted.
	ASSERT(appendFullBlockKey(store, 1).has_value());
	ASSERT(!store.appendKey(ShortKey, sizeof(ShortKey), 2).has_value());

	const std::optional<KeyRef> empty = store.appendKey(nullptr, 0, 3);
	ASSERT(empty.has_value());
	ASSERT(store.isValid(*empty));

	// Reclamation removes storage, not the empty key.
	ASSERT(store.reclaimOldest() == Version{1});
	ASSERT(!store.reclaimOldest().has_value());
	ASSERT(store.isValid(*empty));
	ASSERT(store.keyStart(*empty) == nullptr);
}

TEST_CASE("/resolver/KeyStore/SingleBlockKeys") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const auto first = makeKey(10, 1);
	const auto second = makeKey(7, 2);

	const std::optional<KeyRef> a = append(store, first, 5);
	const std::optional<KeyRef> b = append(store, second, 6);

	ASSERT(a.has_value() && b.has_value());

	// Both keys share the first block and are stored back to back.
	ASSERT(a->firstBlock == b->firstBlock);
	ASSERT_EQ(a->keyOffset, sizeof(KeyBlockHeader));
	ASSERT_EQ(b->keyOffset, sizeof(KeyBlockHeader) + first.size());
	ASSERT_EQ(a->keyLength, first.size());
	ASSERT_EQ(a->blockGen, b->blockGen);

	std::size_t spanned = 0;

	ASSERT(readKey(store, *a, &spanned) == first);
	ASSERT_EQ(spanned, 1);
	ASSERT(readKey(store, *b, &spanned) == second);
	ASSERT_EQ(spanned, 1);

	ASSERT(store.keyStart(*a) ==
		   reinterpret_cast<const std::byte*>(a->firstBlock) + a->keyOffset);
	ASSERT_EQ(store.keyBlock(*a)->generation, a->blockGen);
	ASSERT_EQ(store.keyBlock(*a)->maxCV, Version{6});
}

TEST_CASE("/resolver/KeyStore/MultiBlockKey") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// Start mid-block so the first fragment is partial.
	const auto prefix = makeKey(15, 3);
	const auto key = makeKey(SmallPayload * 3, 4);

	ASSERT(append(store, prefix, 1).has_value());

	const std::optional<KeyRef> ref = append(store, key, 9);
	ASSERT(ref.has_value());
	ASSERT_EQ(ref->keyOffset, sizeof(KeyBlockHeader) + prefix.size());

	// 25 bytes in the first block, then 40 + 40 + 15.
	std::size_t spanned = 0;
	ASSERT(readKey(store, *ref, &spanned) == key);
	ASSERT_EQ(spanned, 4);

	// Every block holding part of the key records its version, and the
	// continuation blocks are newer than the first one.
	const KeyBlockHeader* block = store.keyBlock(*ref);
	for (std::size_t i = 0; i < spanned; ++i) {
		ASSERT_EQ(block->maxCV, Version{9});
		ASSERT(i == 0 || block->generation > ref->blockGen);
		block = block->nextBlock;
	}

	// The next key continues in the last block of the previous one.
	const auto after = makeKey(5, 5);
	const std::optional<KeyRef> next = append(store, after, 10);
	ASSERT(next.has_value());
	ASSERT_EQ(next->keyOffset, sizeof(KeyBlockHeader) + 15);
	ASSERT(readKey(store, *next) == after);
}

TEST_CASE("/resolver/KeyStore/KeyFillsBlockExactly") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// The missing bytes are an exact multiple of the payload.
	const auto key = makeKey(SmallPayload * 2, 6);
	const std::optional<KeyRef> ref = append(store, key, 1);
	ASSERT(ref.has_value());

	std::size_t spanned = 0;
	ASSERT(readKey(store, *ref, &spanned) == key);
	ASSERT_EQ(spanned, 2);

	// The full last block is followed by a fresh open block.
	const auto next = makeKey(3, 7);
	const std::optional<KeyRef> nextRef = append(store, next, 2);
	ASSERT(nextRef.has_value());
	ASSERT_EQ(nextRef->keyOffset, sizeof(KeyBlockHeader));
	ASSERT(nextRef->firstBlock != ref->firstBlock);
	ASSERT(nextRef->blockGen > ref->blockGen);
	ASSERT(readKey(store, *nextRef) == next);
}

// The first extra block cannot be reserved: nothing to give back.
TEST_CASE("/resolver/KeyStore/ReservationFailsOnFirstExtraBlock") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);
	const std::vector<std::byte*> taken = drainPool(pool, 1);

	// Needs a new open block and one more; only one is available.
	ASSERT(!append(store, makeKey(SmallPayload + 1, 8), 1).has_value());
	ASSERT_EQ(freeBlockCount(pool), 1);

	// No block was kept; the next key opens one.
	const auto small = makeKey(4, 9);
	const std::optional<KeyRef> ref = append(store, small, 2);
	ASSERT(ref.has_value());
	ASSERT_EQ(ref->keyOffset, sizeof(KeyBlockHeader));
	ASSERT(readKey(store, *ref) == small);

	for (std::byte* block : taken) {
		pool.releaseBlock(block);
	}
}

// A later extra block cannot be reserved: the earlier ones are given back.
TEST_CASE("/resolver/KeyStore/ReservationFailsOnLaterExtraBlock") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);
	const std::vector<std::byte*> taken = drainPool(pool, 2);

	// Needs three blocks: a new open one and two more; only two are available.
	ASSERT(!append(store, makeKey(SmallPayload * 3, 8), 1).has_value());
	ASSERT_EQ(freeBlockCount(pool), 2);

	const auto small = makeKey(4, 9);
	const std::optional<KeyRef> ref = append(store, small, 2);
	ASSERT(ref.has_value());
	ASSERT_EQ(ref->keyOffset, sizeof(KeyBlockHeader));
	ASSERT(readKey(store, *ref) == small);

	for (std::byte* block : taken) {
		pool.releaseBlock(block);
	}
}

TEST_CASE("/resolver/KeyStore/OldestCV") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// No block is reserved before the first key bytes are stored.
	ASSERT(!store.oldestCV().has_value());
	ASSERT(!store.reclaimOldest().has_value());

	ASSERT(append(store, makeKey(10, 1), 5).has_value());
	ASSERT(append(store, makeKey(10, 2), 7).has_value());
	ASSERT(store.oldestCV() == Version{7});

	// Fill the first block and spill into the second one.
	ASSERT(append(store, makeKey(30, 3), 8).has_value());
	ASSERT(store.oldestCV() == Version{8});

	ASSERT(store.reclaimOldest() == Version{8});
	ASSERT(store.oldestCV() == Version{8});

	ASSERT(store.reclaimOldest() == Version{8});
	ASSERT(!store.oldestCV().has_value());
}

TEST_CASE("/resolver/KeyStore/IsValid") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// a fills the first block exactly; b starts in the second one.
	const std::optional<KeyRef> a = append(store, makeKey(SmallPayload, 1), 1);
	const std::optional<KeyRef> b = append(store, makeKey(10, 2), 2);
	ASSERT(a.has_value() && b.has_value());

	ASSERT(store.isValid(*a));
	ASSERT(store.isValid(*b));

	ASSERT(store.reclaimOldest().has_value());
	ASSERT(!store.isValid(*a));
	ASSERT(store.isValid(*b));

	ASSERT(store.reclaimOldest().has_value());
	ASSERT(!store.isValid(*b));
}

TEST_CASE("/resolver/KeyStore/IsValidMultiBlockKey") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// The key starts in the first block and continues into the next two.
	const std::optional<KeyRef> ref =
		append(store, makeKey(SmallPayload * 2 + 10, 1), 1);
	ASSERT(ref.has_value());
	ASSERT(store.isValid(*ref));

	// Reclaiming the first block invalidates the whole key.
	ASSERT(store.reclaimOldest().has_value());
	ASSERT(!store.isValid(*ref));
}

namespace {

int sign(int value) {
	return (value > 0) - (value < 0);
}

// Byte-wise lexicographic order, the order compareKeys must implement.
int expectedOrder(const std::vector<std::byte>& a,
				  const std::vector<std::byte>& b) {
	if (a < b) {
		return -1;
	}
	return a == b ? 0 : 1;
}

std::vector<std::byte> bytesOf(std::initializer_list<unsigned> values) {
	std::vector<std::byte> bytes;

	for (unsigned value : values) {
		bytes.push_back(static_cast<std::byte>(value));
	}

	return bytes;
}

StringRef asStringRef(const std::vector<std::byte>& bytes) {
	return StringRef(reinterpret_cast<const uint8_t*>(bytes.data()),
					 static_cast<int>(bytes.size()));
}

// Checks the order of a and b in both directions, comparing the stored keys
// with each other and the contiguous bytes of each with the other stored key.
void checkOrder(const KeyStore& store, const KeyRef& a, const KeyRef& b,
				int expected) {
	ASSERT_EQ(sign(store.compareKeys(a, b)), expected);
	ASSERT_EQ(sign(store.compareKeys(b, a)), -expected);

	const std::vector<std::byte> bytesA = readKey(store, a);
	const std::vector<std::byte> bytesB = readKey(store, b);
	ASSERT_EQ(sign(store.compareKeys(asStringRef(bytesA), b)), expected);
	ASSERT_EQ(sign(store.compareKeys(asStringRef(bytesB), a)), -expected);
}

} // namespace

TEST_CASE("/resolver/KeyStore/CompareKeysEqual") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const auto key = makeKey(10, 1);
	const std::optional<KeyRef> a = append(store, key, 1);
	const std::optional<KeyRef> b = append(store, key, 1);
	ASSERT(a.has_value() && b.has_value());

	ASSERT_EQ(store.compareKeys(*a, *a), 0);
	checkOrder(store, *a, *b, 0);
}

TEST_CASE("/resolver/KeyStore/CompareKeysEmpty") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const std::optional<KeyRef> empty1 = store.appendKey(nullptr, 0, 1);
	const std::optional<KeyRef> empty2 = store.appendKey(nullptr, 0, 1);
	const std::optional<KeyRef> smallest = append(store, bytesOf({0x00}), 1);
	ASSERT(empty1.has_value() && empty2.has_value() && smallest.has_value());

	checkOrder(store, *empty1, *empty2, 0);

	// The empty key precedes every other key, even a single zero byte.
	checkOrder(store, *empty1, *smallest, -1);
}

TEST_CASE("/resolver/KeyStore/CompareKeysPrefix") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const std::optional<KeyRef> ab = append(store, bytesOf({'a', 'b'}), 1);
	const std::optional<KeyRef> abc = append(store, bytesOf({'a', 'b', 'c'}), 1);
	const std::optional<KeyRef> ab0 = append(store, bytesOf({'a', 'b', 0}), 1);
	ASSERT(ab.has_value() && abc.has_value() && ab0.has_value());

	// A proper prefix precedes the longer key, whatever the extra bytes.
	checkOrder(store, *ab, *abc, -1);
	checkOrder(store, *ab, *ab0, -1);
	checkOrder(store, *ab0, *abc, -1);
}

TEST_CASE("/resolver/KeyStore/CompareKeysFirstDifferenceDecides") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const auto base = makeKey(12, 3);

	for (std::size_t position : {std::size_t{0}, std::size_t{5}, std::size_t{11}}) {
		auto greater = base;
		greater[position] = static_cast<std::byte>(
			std::to_integer<unsigned>(greater[position]) + 1);

		// Later bytes in the greater key are lower, so only the first
		// difference can decide.
		for (std::size_t i = position + 1; i < greater.size(); ++i) {
			greater[i] = std::byte{0};
		}

		const std::optional<KeyRef> a = append(store, base, 1);
		const std::optional<KeyRef> b = append(store, greater, 1);
		ASSERT(a.has_value() && b.has_value());

		checkOrder(store, *a, *b, expectedOrder(base, greater));
		ASSERT_EQ(expectedOrder(base, greater), -1);
	}
}

TEST_CASE("/resolver/KeyStore/CompareKeysUnsignedBytes") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const std::optional<KeyRef> k00 = append(store, bytesOf({0x00}), 1);
	const std::optional<KeyRef> k7f = append(store, bytesOf({0x7F}), 1);
	const std::optional<KeyRef> k80 = append(store, bytesOf({0x80}), 1);
	const std::optional<KeyRef> kff = append(store, bytesOf({0xFF}), 1);
	ASSERT(k00.has_value() && k7f.has_value() && k80.has_value() &&
		   kff.has_value());

	// Bytes compare as unsigned values: 0x80 and 0xFF are not negative.
	checkOrder(store, *k00, *k7f, -1);
	checkOrder(store, *k7f, *k80, -1);
	checkOrder(store, *k80, *kff, -1);
	checkOrder(store, *k00, *kff, -1);
}

// Keys spanning several blocks, stored at different offsets so that their
// fragment boundaries do not line up.
TEST_CASE("/resolver/KeyStore/CompareKeysMultiBlock") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const auto base = makeKey(SmallPayload * 3 + 7, 5);

	const std::optional<KeyRef> a = append(store, base, 1);
	ASSERT(append(store, makeKey(13, 6), 1).has_value());
	const std::optional<KeyRef> same = append(store, base, 1);
	ASSERT(a.has_value() && same.has_value());
	ASSERT(a->keyOffset != same->keyOffset);

	std::size_t spannedA = 0;
	std::size_t spannedSame = 0;
	ASSERT(readKey(store, *a, &spannedA) == base);
	ASSERT(readKey(store, *same, &spannedSame) == base);
	ASSERT(spannedA >= 4 && spannedSame >= 4);

	checkOrder(store, *a, *same, 0);

	// One difference in each fragment, including the first and last bytes
	// and the bytes on either side of a block boundary.
	const std::size_t positions[] = {
		0,
		SmallPayload - 1,
		SmallPayload,
		SmallPayload * 2 + 3,
		base.size() - 1
	};

	for (std::size_t position : positions) {
		for (int delta : {-1, 1}) {
			auto other = base;
			other[position] = static_cast<std::byte>(
				std::to_integer<int>(other[position]) + delta);

			const std::optional<KeyRef> b = append(store, other, 1);
			ASSERT(b.has_value());

			checkOrder(store, *a, *b, expectedOrder(base, other));
			checkOrder(store, *same, *b, expectedOrder(base, other));
		}
	}

	// A multi-block prefix precedes the longer key.
	const std::vector<std::byte> prefix(base.begin(), base.end() - 1);
	const std::optional<KeyRef> p = append(store, prefix, 1);
	ASSERT(p.has_value());
	checkOrder(store, *p, *a, -1);
}

// Every pair of keys derived from one base key, with shared prefixes of
// every length, single-byte changes and extreme byte values, compared with
// the reference order.
TEST_CASE("/resolver/KeyStore/CompareKeysAgainstReference") {
	PagePool pool(4 * PageSize, SmallBlock);
	KeyStore store(pool);

	const std::byte values[] = {
		std::byte{0x00}, std::byte{0x01}, std::byte{0x7F},
		std::byte{0x80}, std::byte{0xFE}, std::byte{0xFF}
	};

	const auto base = makeKey(SmallPayload * 4, 7);
	std::uint32_t state = 12345;
	auto next = [&state]() {
		state = state * 1103515245u + 12345u;
		return state >> 8;
	};

	std::vector<std::vector<std::byte>> keys;
	std::vector<KeyRef> refs;

	for (int i = 0; i < 80; ++i) {
		const std::size_t length = next() % (base.size() + 1);
		std::vector<std::byte> key(base.begin(), base.begin() + length);

		if (length > 0 && next() % 2 == 0) {
			key[next() % length] = values[next() % std::size(values)];
		}

		const std::optional<KeyRef> ref = append(store, key, 1);
		ASSERT(ref.has_value());
		ASSERT(readKey(store, *ref) == key);

		keys.push_back(key);
		refs.push_back(*ref);
	}

	for (std::size_t i = 0; i < keys.size(); ++i) {
		for (std::size_t j = 0; j < keys.size(); ++j) {
			ASSERT_EQ(sign(store.compareKeys(refs[i], refs[j])),
					  expectedOrder(keys[i], keys[j]));
			ASSERT_EQ(sign(store.compareKeys(asStringRef(keys[i]), refs[j])),
					  expectedOrder(keys[i], keys[j]));
		}
	}
}

namespace {

// Total length of the stored record behind a range.
std::size_t recordLength(const RangeRef& r) {
	return std::size_t{r.endStart} + r.endLength;
}

// Reads length bytes from logical position start of the record, walking
// the blocks independently of KeyStore.
std::vector<std::byte> readRecord(const KeyStore& store, const RangeRef& r,
								  std::size_t start, std::size_t length) {
	std::vector<std::byte> bytes;
	const KeyBlockHeader* block = r.firstBlock;
	std::size_t offset = r.keyOffset;
	std::size_t skip = start + length;
	std::size_t position = 0;

	while (skip != 0) {
		if (offset == store.blockSize()) {
			block = block->nextBlock;
			ASSERT(block != nullptr);
			offset = sizeof(KeyBlockHeader);
		}
		const std::byte* data = reinterpret_cast<const std::byte*>(block);
		if (position >= start) {
			bytes.push_back(data[offset]);
		}
		++offset;
		++position;
		--skip;
	}

	return bytes;
}

// The distinct blocks holding the bytes of a record, in chain order.
std::vector<const KeyBlockHeader*> recordBlocks(const KeyStore& store,
												const RangeRef& r) {
	std::vector<const KeyBlockHeader*> blocks{r.firstBlock};
	const KeyBlockHeader* block = r.firstBlock;
	std::size_t offset = r.keyOffset;

	for (std::size_t i = 0; i < recordLength(r); ++i) {
		if (offset == store.blockSize()) {
			block = block->nextBlock;
			ASSERT(block != nullptr);
			blocks.push_back(block);
			offset = sizeof(KeyBlockHeader);
		}
		++offset;
	}

	return blocks;
}

// appendRange requires begin < end; the tests check it before calling.
std::optional<RangeRef> appendRange(KeyStore& store,
									const std::vector<std::byte>& begin,
									const std::vector<std::byte>& end,
									Version cv) {
	ASSERT(begin < end);
	return store.appendRange(asStringRef(begin), asStringRef(end), cv);
}

// Checks that both views of the range read back the given keys.
void checkRangeBytes(const KeyStore& store, const RangeRef& r,
					 const std::vector<std::byte>& begin,
					 const std::vector<std::byte>& end) {
	ASSERT_EQ(r.beginLength, begin.size());
	ASSERT_EQ(r.endLength, end.size());
	ASSERT(readRecord(store, r, 0, r.beginLength) == begin);
	ASSERT(readRecord(store, r, r.endStart, r.endLength) == end);
	ASSERT_EQ(store.compareBegin(asStringRef(begin), r), 0);
	ASSERT_EQ(store.compareEnd(asStringRef(end), r), 0);
}

} // namespace

TEST_CASE("/resolver/KeyStore/AppendRangePointStoresKeyOnce") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const auto begin = bytesOf({'a', 'b', 'c'});
	const auto end = bytesOf({'a', 'b', 'c', 0});
	const std::optional<RangeRef> r = appendRange(store, begin, end, 7);
	ASSERT(r.has_value());

	ASSERT_EQ(r->keyOffset, sizeof(KeyBlockHeader));
	ASSERT_EQ(r->beginLength, 3);
	ASSERT_EQ(r->endStart, 0);
	ASSERT_EQ(r->endLength, 4);
	ASSERT_EQ(recordLength(*r), 4);
	checkRangeBytes(store, *r, begin, end);

	// The next record starts right after the four stored bytes.
	const std::optional<KeyRef> next = append(store, bytesOf({'z'}), 7);
	ASSERT(next.has_value());
	ASSERT_EQ(next->keyOffset, sizeof(KeyBlockHeader) + 4);
}

TEST_CASE("/resolver/KeyStore/AppendRangeGeneralStoresBeginThenEnd") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const auto begin = bytesOf({'a', 'b', 'c'});
	const auto end = bytesOf({'x', 'y', 'z'});
	const std::optional<RangeRef> r = appendRange(store, begin, end, 7);
	ASSERT(r.has_value());

	ASSERT_EQ(r->beginLength, 3);
	ASSERT_EQ(r->endStart, 3);
	ASSERT_EQ(r->endLength, 3);
	ASSERT(readRecord(store, *r, 0, 6) == bytesOf({'a', 'b', 'c', 'x', 'y', 'z'}));
	checkRangeBytes(store, *r, begin, end);

	// A key followed by a non-zero byte is a general range, not a point.
	const auto almostPoint = bytesOf({'a', 'b', 'c', 1});
	const std::optional<RangeRef> g = appendRange(store, begin, almostPoint, 7);
	ASSERT(g.has_value());
	ASSERT_EQ(g->endStart, 3);
	ASSERT_EQ(g->keyOffset, r->keyOffset + 6);
	checkRangeBytes(store, *g, begin, almostPoint);

	// So is an end one byte longer and ending in zero that does not start
	// with begin.
	const auto otherPrefix = bytesOf({'a', 'b', 'd', 0});
	const std::optional<RangeRef> h = appendRange(store, begin, otherPrefix, 7);
	ASSERT(h.has_value());
	ASSERT_EQ(h->endStart, 3);
	checkRangeBytes(store, *h, begin, otherPrefix);
}

TEST_CASE("/resolver/KeyStore/AppendRangeEmptyBegin") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const std::vector<std::byte> empty;
	const std::optional<RangeRef> point =
		appendRange(store, empty, bytesOf({0}), 3);
	const std::optional<RangeRef> general =
		appendRange(store, empty, bytesOf({'x'}), 3);
	ASSERT(point.has_value() && general.has_value());

	// Both store only the end, and begin is the empty prefix of it.
	ASSERT_EQ(point->beginLength, 0);
	ASSERT_EQ(point->endStart, 0);
	ASSERT_EQ(point->endLength, 1);
	ASSERT_EQ(general->beginLength, 0);
	ASSERT_EQ(general->endStart, 0);
	ASSERT_EQ(general->endLength, 1);
	ASSERT_EQ(general->keyOffset, point->keyOffset + 1);
	checkRangeBytes(store, *point, empty, bytesOf({0}));
	checkRangeBytes(store, *general, empty, bytesOf({'x'}));

	// An empty begin does not make the range valid forever.
	const KeyRef begin = store.beginKey(*general);
	ASSERT_EQ(begin.keyLength, 0);
	ASSERT(store.isValid(*general));

	ASSERT(store.reclaimOldest() == Version{3});
	ASSERT(!store.isValid(*point));
	ASSERT(!store.isValid(*general));
	ASSERT(store.isValid(begin));
}

TEST_CASE("/resolver/KeyStore/BeginKeyViewsTheRecord") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	ASSERT(append(store, makeKey(9, 1), 1).has_value());
	const auto begin = makeKey(SmallPayload + 5, 2);
	const auto end = makeKey(SmallPayload, 3);
	const std::optional<RangeRef> r = appendRange(store, begin, end, 4);
	ASSERT(r.has_value());

	const KeyRef key = store.beginKey(*r);
	ASSERT(key.firstBlock == r->firstBlock);
	ASSERT_EQ(key.keyOffset, r->keyOffset);
	ASSERT_EQ(key.keyLength, r->beginLength);
	ASSERT_EQ(key.blockGen, r->blockGen);
	ASSERT(readKey(store, key) == begin);
}

// Records whose end starts exactly at the first byte of the next block, one
// byte before it and one byte after it.
TEST_CASE("/resolver/KeyStore/AppendRangeEndAtBlockBoundary") {
	for (std::size_t beginLength :
		 {SmallPayload - 1, SmallPayload, SmallPayload + 1}) {
		PagePool pool(PageSize, SmallBlock);
		KeyStore store(pool);

		const auto begin = makeKey(beginLength, 4);
		auto end = begin;
		end.push_back(std::byte{7});
		end.push_back(std::byte{8});

		const std::optional<RangeRef> r = appendRange(store, begin, end, 2);
		ASSERT(r.has_value());
		ASSERT_EQ(r->keyOffset, sizeof(KeyBlockHeader));
		ASSERT_EQ(r->endStart, beginLength);
		checkRangeBytes(store, *r, begin, end);

		// When begin fills the first block, end starts at the first data
		// byte of the second one.
		if (beginLength == SmallPayload) {
			const std::vector<const KeyBlockHeader*> blocks =
				recordBlocks(store, *r);
			ASSERT(blocks.size() >= 2);
			ASSERT(reinterpret_cast<const std::byte*>(blocks[1])
					   [sizeof(KeyBlockHeader)] == end.front());
		}

		// Keys differing in the first and the last byte of the end.
		auto lower = end;
		lower.front() = static_cast<std::byte>(
			std::to_integer<int>(lower.front()) - 1);
		auto higher = end;
		higher.back() = static_cast<std::byte>(
			std::to_integer<int>(higher.back()) + 1);
		ASSERT(store.compareEnd(asStringRef(lower), *r) < 0);
		ASSERT(store.compareEnd(asStringRef(higher), *r) > 0);
		ASSERT(store.compareEnd(asStringRef(begin), *r) < 0);
	}
}

TEST_CASE("/resolver/KeyStore/AppendRangeAcrossBlocks") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	ASSERT(append(store, makeKey(13, 1), 1).has_value());

	const auto begin = makeKey(SmallPayload * 2 - 3, 5);
	auto end = begin;
	end.back() = static_cast<std::byte>(std::to_integer<int>(end.back()) + 1);
	end.insert(end.end(), 50, std::byte{9});

	const std::optional<RangeRef> r = appendRange(store, begin, end, 6);
	ASSERT(r.has_value());
	ASSERT(r->keyOffset != sizeof(KeyBlockHeader));
	ASSERT(recordBlocks(store, *r).size() >= 5);
	checkRangeBytes(store, *r, begin, end);

	// Every block of the record records the range's CV.
	for (const KeyBlockHeader* block : recordBlocks(store, *r)) {
		ASSERT(block->maxCV >= Version{6});
	}
}

TEST_CASE("/resolver/KeyStore/AppendRangeIsAllOrNothing") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const std::optional<RangeRef> first =
		appendRange(store, bytesOf({'a'}), bytesOf({'b'}), 1);
	ASSERT(first.has_value());
	const std::vector<std::byte*> taken = drainPool(pool, 1);

	// Needs the open block and two more; only one is available.
	const auto begin = makeKey(SmallPayload, 1);
	const auto end = makeKey(SmallPayload + 1, 2);
	ASSERT(!appendRange(store, begin, end, 2).has_value());
	ASSERT_EQ(freeBlockCount(pool), 1);

	// The open block is untouched: the next range follows the first one.
	const std::optional<RangeRef> next =
		appendRange(store, bytesOf({'c'}), bytesOf({'d'}), 3);
	ASSERT(next.has_value());
	ASSERT(next->firstBlock == first->firstBlock);
	ASSERT_EQ(next->keyOffset, first->keyOffset + 2);
	checkRangeBytes(store, *first, bytesOf({'a'}), bytesOf({'b'}));

	for (std::byte* block : taken) {
		pool.releaseBlock(block);
	}
}

// Random points and general ranges, some spanning blocks, compared with
// random external keys through both views and against beginKey.
TEST_CASE("/resolver/KeyStore/CompareRangeEndsAgainstReference") {
	PagePool pool(8 * PageSize, SmallBlock);
	KeyStore store(pool);

	const std::byte values[] = {
		std::byte{0x00}, std::byte{0x01}, std::byte{0x7F},
		std::byte{0x80}, std::byte{0xFF}
	};
	const auto base = makeKey(SmallPayload * 3, 11);
	std::uint32_t state = 2026;
	auto next = [&state]() {
		state = state * 1103515245u + 12345u;
		return state >> 8;
	};
	auto derived = [&]() {
		std::vector<std::byte> key(base.begin(),
								   base.begin() + next() % (base.size() + 1));
		if (!key.empty() && next() % 2 == 0) {
			key[next() % key.size()] = values[next() % std::size(values)];
		}
		return key;
	};

	struct Stored {
		RangeRef ref;
		std::vector<std::byte> begin;
		std::vector<std::byte> end;
	};
	std::vector<Stored> ranges;

	while (ranges.size() < 60) {
		std::vector<std::byte> begin = derived();
		std::vector<std::byte> end;

		if (next() % 2 == 0) {
			end = begin;
			end.push_back(std::byte{0});
		} else {
			end = derived();
			if (!(begin < end)) {
				continue;
			}
		}

		const std::optional<RangeRef> r = appendRange(store, begin, end, 1);
		ASSERT(r.has_value());
		checkRangeBytes(store, *r, begin, end);
		ranges.push_back({*r, begin, end});
	}

	std::vector<std::vector<std::byte>> keys;
	for (int i = 0; i < 60; ++i) {
		keys.push_back(derived());
	}
	for (const Stored& s : ranges) {
		keys.push_back(s.begin);
		keys.push_back(s.end);
	}

	for (const Stored& s : ranges) {
		for (const auto& key : keys) {
			ASSERT_EQ(sign(store.compareBegin(asStringRef(key), s.ref)),
					  expectedOrder(key, s.begin));
			ASSERT_EQ(sign(store.compareEnd(asStringRef(key), s.ref)),
					  expectedOrder(key, s.end));
			ASSERT_EQ(sign(store.compareKeys(asStringRef(key),
											 store.beginKey(s.ref))),
					  expectedOrder(key, s.begin));
		}
	}
}

// Trimming copies the outer parts of a range as new records with their old
// CV. Closed blocks are not modified, new blocks record the maximum CV of
// their records, and reclamation stays FIFO with validity by generation.
TEST_CASE("/resolver/KeyStore/CopiesWithOldCVKeepOldBlocksIntact") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// [a, z) with CV 10 fills its block exactly, which closes it.
	auto a = makeKey(SmallPayload / 2, 1);
	auto z = a;
	z.front() = std::byte{0xFF};
	const std::optional<RangeRef> old = appendRange(store, a, z, 10);
	ASSERT(old.has_value());
	ASSERT_EQ(recordLength(*old), SmallPayload);
	ASSERT_EQ(recordBlocks(store, *old).size(), 1);

	const KeyBlockHeader* oldBlock = old->firstBlock;
	const std::vector<std::byte> oldContents(
		reinterpret_cast<const std::byte*>(oldBlock),
		reinterpret_cast<const std::byte*>(oldBlock) + SmallBlock);
	ASSERT_EQ(oldBlock->maxCV, Version{10});

	// The new write [d, m) with CV 20, and the outer parts [a, d) and
	// [m, z) copied with CV 10. One more copy carries an even older CV.
	auto d = a;
	d.front() = std::byte{0x40};
	auto m = a;
	m.front() = std::byte{0x80};

	const std::optional<RangeRef> middle = appendRange(store, d, m, 20);
	const std::optional<RangeRef> left = appendRange(store, a, d, 10);
	const std::optional<RangeRef> right = appendRange(store, m, z, 10);
	const std::optional<RangeRef> older =
		appendRange(store, bytesOf({'p'}), bytesOf({'p', 0}), 5);
	ASSERT(middle.has_value() && left.has_value() && right.has_value() &&
		   older.has_value());

	// The old block is unchanged, including its maximum CV.
	ASSERT(std::equal(oldContents.begin(), oldContents.end(),
					  reinterpret_cast<const std::byte*>(oldBlock)));
	ASSERT_EQ(oldBlock->maxCV, Version{10});
	checkRangeBytes(store, *old, a, z);

	// Every new record is after the old block and has a newer generation.
	const RangeRef copies[] = {*middle, *left, *right, *older};
	const Version cvs[] = {20, 10, 10, 5};
	for (const RangeRef& r : copies) {
		ASSERT(r.firstBlock != oldBlock);
		ASSERT(r.blockGen > old->blockGen);
	}
	checkRangeBytes(store, *middle, d, m);
	checkRangeBytes(store, *left, a, d);
	checkRangeBytes(store, *right, m, z);

	// Each new block holds the maximum CV of the records it contains.
	std::vector<std::pair<const KeyBlockHeader*, Version>> expectedMax;
	for (std::size_t i = 0; i < std::size(copies); ++i) {
		for (const KeyBlockHeader* block : recordBlocks(store, copies[i])) {
			auto found = std::find_if(
				expectedMax.begin(), expectedMax.end(),
				[block](const auto& e) { return e.first == block; });
			if (found == expectedMax.end()) {
				expectedMax.push_back({block, cvs[i]});
			} else {
				found->second = std::max(found->second, cvs[i]);
			}
		}
	}
	ASSERT(expectedMax.size() >= 2);
	for (const auto& [block, cv] : expectedMax) {
		ASSERT_EQ(block->maxCV, cv);
	}

	// Reclamation is FIFO: the old block first, with its own maximum CV.
	ASSERT(store.oldestCV() == Version{10});
	ASSERT(store.reclaimOldest() == Version{10});
	ASSERT(!store.isValid(*old));
	for (const RangeRef& r : copies) {
		ASSERT(store.isValid(r));
	}

	// Then the new blocks, in chain order, each with its maximum CV. A
	// record becomes invalid when its first block is reclaimed.
	for (const auto& [block, cv] : expectedMax) {
		ASSERT(store.reclaimOldest() == cv);
		for (const RangeRef& r : copies) {
			if (r.firstBlock == block) {
				ASSERT(!store.isValid(r));
			}
		}
	}
}

namespace {

bool isPoint(const std::vector<std::byte>& begin,
			 const std::vector<std::byte>& end) {
	auto expected = begin;
	expected.push_back(std::byte{0});
	return end == expected;
}

// [source.begin, end); requires a non-empty range.
std::optional<RangeRef> appendKeepingBegin(KeyStore& store,
										   const RangeRef& source,
										   const std::vector<std::byte>& end,
										   Version cv) {
	ASSERT(readRecord(store, source, 0, source.beginLength) < end);
	return store.appendRange(source, asStringRef(end), cv);
}

// [begin, source.end); requires a non-empty range.
std::optional<RangeRef> appendKeepingEnd(KeyStore& store,
										 const std::vector<std::byte>& begin,
										 const RangeRef& source, Version cv) {
	ASSERT(begin < readRecord(store, source, source.endStart, source.endLength));
	return store.appendRange(asStringRef(begin), source, cv);
}

// Checks a new range: both views read back, a point is stored once, and a
// general range stores begin followed by end.
void checkNewRange(const KeyStore& store, const RangeRef& r,
				   const std::vector<std::byte>& begin,
				   const std::vector<std::byte>& end) {
	checkRangeBytes(store, r, begin, end);
	ASSERT_EQ(r.endStart, isPoint(begin, end) ? 0 : begin.size());
}

RangeRef storeRange(KeyStore& store, const std::vector<std::byte>& begin,
					const std::vector<std::byte>& end, Version cv) {
	const std::optional<RangeRef> r = appendRange(store, begin, end, cv);
	ASSERT(r.has_value());
	return *r;
}

} // namespace

// The left exterior [source.begin, end) of a trimmed range.
TEST_CASE("/resolver/KeyStore/AppendRangeKeepsSourceBegin") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// A source whose begin spans blocks.
	const auto longBegin = makeKey(SmallPayload * 2 + 7, 1);
	auto longEnd = longBegin;
	longEnd.front() = std::byte{0xFF};
	const RangeRef wide = storeRange(store, longBegin, longEnd, 5);
	auto cut = longBegin;
	cut.push_back(std::byte{5});
	const std::optional<RangeRef> left = appendKeepingBegin(store, wide, cut, 5);
	ASSERT(left.has_value());
	ASSERT(left->blockGen > wide.blockGen);
	checkNewRange(store, *left, longBegin, cut);
	checkRangeBytes(store, wide, longBegin, longEnd);

	// A point source keeps its begin; a new end equal to begin + 0 makes a
	// point, stored once.
	const auto abc = bytesOf({'a', 'b', 'c'});
	const RangeRef point = storeRange(store, abc, bytesOf({'a', 'b', 'c', 0}), 5);
	const std::optional<RangeRef> r1 =
		appendKeepingBegin(store, point, bytesOf({'a', 'b', 'c', 0}), 5);
	ASSERT(r1.has_value());
	checkNewRange(store, *r1, abc, bytesOf({'a', 'b', 'c', 0}));

	// One byte longer and ending in zero, but not a point.
	for (const auto& end : {bytesOf({'a', 'b', 'c', 1}),
							bytesOf({'a', 'b', 'd', 0}),
							bytesOf({'z'})}) {
		const std::optional<RangeRef> r =
			appendKeepingBegin(store, point, end, 5);
		ASSERT(r.has_value());
		checkNewRange(store, *r, abc, end);
	}

	// A source with an empty begin.
	const RangeRef fromEmpty = storeRange(store, {}, bytesOf({'q'}), 5);
	for (const auto& end : {bytesOf({0}), bytesOf({'x'})}) {
		const std::optional<RangeRef> r =
			appendKeepingBegin(store, fromEmpty, end, 5);
		ASSERT(r.has_value());
		checkNewRange(store, *r, {}, end);
	}
}

// The right exterior [begin, source.end) of a trimmed range.
TEST_CASE("/resolver/KeyStore/AppendRangeKeepsSourceEnd") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// A source whose end starts after a begin spanning blocks.
	const auto longBegin = makeKey(SmallPayload + 9, 1);
	auto longEnd = makeKey(SmallPayload * 2 + 3, 2);
	longEnd.front() = std::byte{0xFF};
	const RangeRef wide = storeRange(store, longBegin, longEnd, 5);
	auto cut = longEnd;
	cut.back() = std::byte{0};
	cut.front() = std::byte{0x80};
	const std::optional<RangeRef> right = appendKeepingEnd(store, cut, wide, 5);
	ASSERT(right.has_value());
	checkNewRange(store, *right, cut, longEnd);
	checkRangeBytes(store, wide, longBegin, longEnd);

	// A point source keeps its end: with the same begin, the result is the
	// same point, copied from the store.
	const auto abc = bytesOf({'a', 'b', 'c'});
	const auto abc0 = bytesOf({'a', 'b', 'c', 0});
	const RangeRef point = storeRange(store, abc, abc0, 5);
	const std::optional<RangeRef> r1 = appendKeepingEnd(store, abc, point, 5);
	ASSERT(r1.has_value());
	checkNewRange(store, *r1, abc, abc0);

	// Not points: other lengths, a last byte that is not zero, or an end
	// that does not start with begin.
	const std::pair<std::vector<std::byte>, std::vector<std::byte>> cases[] = {
		{bytesOf({'a', 'b', 'c'}), bytesOf({'x', 'y', 'z'})},
		{bytesOf({'a', 'b', 'c'}), bytesOf({'a', 'b', 'c', 1})},
		{bytesOf({'a', 'b', 'c'}), bytesOf({'a', 'b', 'd', 0})},
		{{}, bytesOf({'x'})},
		{{}, bytesOf({0})},
	};
	for (const auto& [begin, end] : cases) {
		const RangeRef source = storeRange(store, bytesOf({}), end, 5);
		const std::optional<RangeRef> r = appendKeepingEnd(store, begin, source, 5);
		ASSERT(r.has_value());
		checkNewRange(store, *r, begin, end);
	}
}

// The source's end starts in the next block, and the zero that makes the
// new range a point is in that block too.
TEST_CASE("/resolver/KeyStore/AppendRangeSourceEndInNextBlock") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	// begin fills the first block exactly, so end starts in the next one.
	const auto begin = makeKey(SmallPayload, 2);
	auto end = begin;
	end.front() = std::byte{0xFF};
	end.push_back(std::byte{0});
	const RangeRef source = storeRange(store, begin, end, 5);
	ASSERT_EQ(source.endStart, SmallPayload);

	const std::vector<std::byte> newBegin(end.begin(), end.end() - 1);
	const std::optional<RangeRef> point =
		appendKeepingEnd(store, newBegin, source, 5);
	ASSERT(point.has_value());
	ASSERT_EQ(point->endStart, 0);
	checkNewRange(store, *point, newBegin, end);
}

// Trimming [a, z) with CV 10 by a write [d, m) with CV 20.
TEST_CASE("/resolver/KeyStore/AppendRangeTrimsSource") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const auto a = makeKey(SmallPayload / 2 + 3, 1);
	auto z = a;
	z.front() = std::byte{0xFF};
	const RangeRef old = storeRange(store, a, z, 10);

	auto d = a;
	d.front() = std::byte{0x40};
	auto m = a;
	m.front() = std::byte{0x80};

	const std::optional<RangeRef> left = appendKeepingBegin(store, old, d, 10);
	const std::optional<RangeRef> middle = appendRange(store, d, m, 20);
	const std::optional<RangeRef> right = appendKeepingEnd(store, m, old, 10);
	ASSERT(left.has_value() && middle.has_value() && right.has_value());

	checkNewRange(store, *left, a, d);
	checkNewRange(store, *middle, d, m);
	checkNewRange(store, *right, m, z);
	checkRangeBytes(store, old, a, z);
}

TEST_CASE("/resolver/KeyStore/AppendRangeFromSourceIsAllOrNothing") {
	PagePool pool(PageSize, SmallBlock);
	KeyStore store(pool);

	const auto begin = makeKey(SmallPayload + 5, 3);
	auto end = begin;
	end.front() = std::byte{0xFF};
	const RangeRef source = storeRange(store, begin, end, 2);
	auto cut = begin;
	cut.push_back(std::byte{1});
	const std::vector<std::byte*> taken = drainPool(pool, 1);

	// Each needs the open block and two more; only one is available.
	ASSERT(!appendKeepingBegin(store, source, cut, 2).has_value());
	ASSERT(!appendKeepingEnd(store, cut, source, 2).has_value());
	ASSERT_EQ(freeBlockCount(pool), 1);
	checkRangeBytes(store, source, begin, end);

	for (std::byte* block : taken) {
		pool.releaseBlock(block);
	}
}

// Random sources, points and general ranges, some spanning blocks, trimmed
// on either side by random keys.
TEST_CASE("/resolver/KeyStore/AppendRangeFromSourceAgainstReference") {
	PagePool pool(16 * PageSize, SmallBlock);
	KeyStore store(pool);

	const auto base = makeKey(SmallPayload * 3, 13);
	std::uint32_t state = 77;
	auto next = [&state]() {
		state = state * 1103515245u + 12345u;
		return state >> 8;
	};
	auto derived = [&]() {
		std::vector<std::byte> key(base.begin(),
								   base.begin() + next() % (base.size() + 1));
		if (!key.empty() && next() % 3 == 0) {
			key[next() % key.size()] = std::byte{0};
		}
		return key;
	};
	auto randomRange = [&]() {
		for (;;) {
			std::vector<std::byte> begin = derived();
			std::vector<std::byte> end;
			if (next() % 2 == 0) {
				end = begin;
				end.push_back(std::byte{0});
			} else {
				end = derived();
			}
			if (begin < end) {
				return std::make_pair(begin, end);
			}
		}
	};

	int points = 0;
	for (int n = 0; n < 120;) {
		const auto [sourceBegin, sourceEnd] = randomRange();
		const RangeRef source = storeRange(store, sourceBegin, sourceEnd, 1);
		const auto [begin, end] = randomRange();

		std::optional<RangeRef> r;
		if (n % 2 == 0) {
			if (!(sourceBegin < end)) {
				continue;
			}
			r = appendKeepingBegin(store, source, end, 1);
			ASSERT(r.has_value());
			checkNewRange(store, *r, sourceBegin, end);
			points += isPoint(sourceBegin, end);
		} else {
			if (!(begin < sourceEnd)) {
				continue;
			}
			r = appendKeepingEnd(store, begin, source, 1);
			ASSERT(r.has_value());
			checkNewRange(store, *r, begin, sourceEnd);
			points += isPoint(begin, sourceEnd);
		}
		checkRangeBytes(store, source, sourceBegin, sourceEnd);
		++n;
	}
	ASSERT(points > 0 && points < 120);
}
