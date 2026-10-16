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
