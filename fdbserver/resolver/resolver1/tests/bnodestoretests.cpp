#include "test.h"
#include "bnodestore.h"

#include <algorithm>
#include <vector>

namespace {

// Walks the age list from its oldest node, starting at any linked node.
std::vector<const BNodeHeader*> ageList(const BNodeHeader* node) {
	while (node->agePrevious != nullptr) {
		node = node->agePrevious;
	}

	std::vector<const BNodeHeader*> nodes;

	for (; node != nullptr; node = node->ageNext) {
		nodes.push_back(node);
	}

	return nodes;
}

std::size_t freeBlockCount(PagePool& pool) {
	std::vector<std::byte*> taken;

	for (std::byte* block = pool.allocateBlock(); block != nullptr;
		 block = pool.allocateBlock()) {
		taken.push_back(block);
	}

	for (std::byte* block : taken) {
		pool.releaseBlock(block);
	}

	return taken.size();
}

template<std::size_t C>
struct SmallLayout {
	std::byte data[C];
};

template<std::size_t C>
struct OversizedLayout {
	std::byte data[BlockSize + C];
};

} // namespace

// Called at run time so that every outcome of the search is executed.
TEST_CASE("/resolver/BNodeStore/CapacityForBlock") {
	std::size_t internal = capacityForBlock<InternalBlockLayout>();
	std::size_t leaf = capacityForBlock<LeafBlockLayout>();

	ASSERT_EQ(internal, InternalCapacity);
	ASSERT_EQ(leaf, LeafCapacity);

	// The largest capacity that fits: one more entry does not.
	ASSERT(sizeof(InternalBlockLayout<InternalCapacity>) <= BlockSize);
	ASSERT(sizeof(InternalBlockLayout<InternalCapacity + 1>) > BlockSize);
	ASSERT(sizeof(LeafBlockLayout<LeafCapacity>) <= BlockSize);
	ASSERT(sizeof(LeafBlockLayout<LeafCapacity + 1>) > BlockSize);

	// Capped at 64 slots, and 0 when not even one entry fits.
	std::size_t small = capacityForBlock<SmallLayout>();
	std::size_t oversized = capacityForBlock<OversizedLayout>();

	ASSERT_EQ(small, 64);
	ASSERT_EQ(oversized, 0);
}

TEST_CASE("/resolver/BNodeStore/AllocateLeafInitializesNode") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	LeafBlock* leaf = store.allocateLeaf(7);
	ASSERT(leaf != nullptr);

	ASSERT(leaf->header.kind == BNodeKind::Leaf);
	ASSERT_EQ(leaf->header.generation, 1);
	ASSERT_EQ(leaf->header.maxCV, Version{7});
	ASSERT_EQ(leaf->header.count, 0);
	ASSERT(leaf->header.parent == nullptr);
	ASSERT_EQ(leaf->header.parentSlot, 0);
	ASSERT(leaf->header.agePrevious == nullptr);
	ASSERT(leaf->header.ageNext == nullptr);

	ASSERT_EQ(leaf->header.freeSlots, LeafSlotMask);
	ASSERT(leaf->left == nullptr);
	ASSERT(leaf->right == nullptr);

	for (std::size_t i = 0; i < LeafCapacity; ++i) {
		ASSERT_EQ(leaf->header.order[i], 0);
	}
}

TEST_CASE("/resolver/BNodeStore/AllocateInternalInitializesNode") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	InternalBlock* internal = store.allocateInternal(9);
	ASSERT(internal != nullptr);

	ASSERT(internal->header.kind == BNodeKind::Internal);
	ASSERT_EQ(internal->header.generation, 1);
	ASSERT_EQ(internal->header.maxCV, Version{9});
	ASSERT_EQ(internal->header.count, 0);
	ASSERT(internal->header.parent == nullptr);
	ASSERT_EQ(internal->header.parentSlot, 0);
	ASSERT(internal->header.agePrevious == nullptr);
	ASSERT(internal->header.ageNext == nullptr);

	ASSERT_EQ(internal->header.freeSlots, InternalSlotMask);

	for (std::size_t i = 0; i < InternalCapacity; ++i) {
		ASSERT_EQ(internal->header.order[i], 0);
	}
}

TEST_CASE("/resolver/BNodeStore/GenerationsIncreaseAcrossKinds") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	LeafBlock* a = store.allocateLeaf(1);
	InternalBlock* b = store.allocateInternal(1);
	LeafBlock* c = store.allocateLeaf(2);

	ASSERT_EQ(a->header.generation, 1);
	ASSERT_EQ(b->header.generation, 2);
	ASSERT_EQ(c->header.generation, 3);
}

TEST_CASE("/resolver/BNodeStore/AgeListFollowsAllocationOrder") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	BNodeHeader* a = &store.allocateLeaf(1)->header;
	BNodeHeader* b = &store.allocateInternal(2)->header;
	BNodeHeader* c = &store.allocateLeaf(2)->header;

	const std::vector<const BNodeHeader*> expected = {a, b, c};
	ASSERT(ageList(b) == expected);

	ASSERT(a->agePrevious == nullptr);
	ASSERT(a->ageNext == b);
	ASSERT(b->agePrevious == a);
	ASSERT(b->ageNext == c);
	ASSERT(c->agePrevious == b);
	ASSERT(c->ageNext == nullptr);
}

// A failed allocation takes no block, consumes no generation and leaves
// the age list unchanged.
TEST_CASE("/resolver/BNodeStore/AllocationFailsWhenPoolExhausted") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);
	const std::size_t blocks = PageSize / BlockSize;

	std::vector<BNodeHeader*> nodes;
	for (std::size_t i = 0; i < blocks; ++i) {
		nodes.push_back(&store.allocateLeaf(1)->header);
	}

	ASSERT(store.allocateLeaf(1) == nullptr);
	ASSERT(store.allocateInternal(1) == nullptr);
	ASSERT(nodes.back()->ageNext == nullptr);
	ASSERT_EQ(ageList(nodes.front()).size(), blocks);

	store.releaseNode(nodes.back());
	nodes.pop_back();

	InternalBlock* internal = store.allocateInternal(1);
	ASSERT(internal != nullptr);
	ASSERT_EQ(internal->header.generation, blocks + 1);
	ASSERT(internal->header.agePrevious == nodes.back());
}

TEST_CASE("/resolver/BNodeStore/ReleaseReturnsBlockToPool") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);
	const std::size_t freeBefore = freeBlockCount(pool);

	LeafBlock* leaf = store.allocateLeaf(1);
	ASSERT_EQ(freeBlockCount(pool), freeBefore - 1);

	const auto oldGeneration = leaf->header.generation;
	store.releaseNode(&leaf->header);

	// The block is reused for a node of the other kind.
	InternalBlock* internal = store.allocateInternal(2);
	ASSERT(reinterpret_cast<std::byte*>(internal) ==
		   reinterpret_cast<std::byte*>(leaf));
	ASSERT(internal->header.kind == BNodeKind::Internal);
	ASSERT(internal->header.generation > oldGeneration);
	ASSERT_EQ(internal->header.freeSlots, InternalSlotMask);

	store.releaseNode(&internal->header);
	ASSERT_EQ(freeBlockCount(pool), freeBefore);
}

TEST_CASE("/resolver/BNodeStore/ReleaseOnlyNode") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	store.releaseNode(&store.allocateLeaf(1)->header);

	// The list is empty: the next node has no predecessor.
	LeafBlock* next = store.allocateLeaf(2);
	ASSERT(next->header.agePrevious == nullptr);
	ASSERT(next->header.ageNext == nullptr);
}

TEST_CASE("/resolver/BNodeStore/ReleaseOldestNode") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	BNodeHeader* a = &store.allocateLeaf(1)->header;
	BNodeHeader* b = &store.allocateLeaf(2)->header;
	BNodeHeader* c = &store.allocateLeaf(3)->header;

	store.releaseNode(a);

	const std::vector<const BNodeHeader*> expected = {b, c};
	ASSERT(ageList(c) == expected);
	ASSERT(b->agePrevious == nullptr);
}

TEST_CASE("/resolver/BNodeStore/ReleaseMiddleNode") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	BNodeHeader* a = &store.allocateLeaf(1)->header;
	BNodeHeader* b = &store.allocateLeaf(2)->header;
	BNodeHeader* c = &store.allocateLeaf(3)->header;

	store.releaseNode(b);

	const std::vector<const BNodeHeader*> expected = {a, c};
	ASSERT(ageList(a) == expected);
	ASSERT(a->ageNext == c);
	ASSERT(c->agePrevious == a);
}

TEST_CASE("/resolver/BNodeStore/ReleaseNewestNode") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	BNodeHeader* a = &store.allocateLeaf(1)->header;
	BNodeHeader* b = &store.allocateLeaf(2)->header;

	store.releaseNode(b);
	ASSERT(a->ageNext == nullptr);

	// The previous node is the newest again.
	BNodeHeader* c = &store.allocateLeaf(3)->header;
	ASSERT(c->agePrevious == a);
	ASSERT(a->ageNext == c);
}

TEST_CASE("/resolver/BNodeStore/UpdateCVSameValueKeepsPosition") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	BNodeHeader* a = &store.allocateLeaf(4)->header;
	BNodeHeader* b = &store.allocateLeaf(4)->header;

	store.updateCV(a, 4);

	const std::vector<const BNodeHeader*> expected = {a, b};
	ASSERT(ageList(a) == expected);
	ASSERT_EQ(a->maxCV, Version{4});
}

TEST_CASE("/resolver/BNodeStore/UpdateCVNewestNodeStaysNewest") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	BNodeHeader* a = &store.allocateLeaf(1)->header;
	BNodeHeader* b = &store.allocateLeaf(2)->header;

	store.updateCV(b, 5);

	const std::vector<const BNodeHeader*> expected = {a, b};
	ASSERT(ageList(a) == expected);
	ASSERT_EQ(b->maxCV, Version{5});
}

TEST_CASE("/resolver/BNodeStore/UpdateCVMovesNodeToNewest") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	BNodeHeader* a = &store.allocateLeaf(1)->header;
	BNodeHeader* b = &store.allocateInternal(2)->header;
	BNodeHeader* c = &store.allocateLeaf(3)->header;

	// The oldest node moves to the end.
	store.updateCV(a, 5);
	const std::vector<const BNodeHeader*> afterOldest = {b, c, a};
	ASSERT(ageList(c) == afterOldest);
	ASSERT_EQ(a->maxCV, Version{5});

	// A middle node moves to the end.
	store.updateCV(c, 6);
	const std::vector<const BNodeHeader*> afterMiddle = {b, a, c};
	ASSERT(ageList(a) == afterMiddle);
	ASSERT_EQ(c->maxCV, Version{6});

	// The moved node is the newest: the next node follows it.
	BNodeHeader* d = &store.allocateLeaf(6)->header;
	ASSERT(d->agePrevious == c);

	// Generations keep their allocation order.
	ASSERT_EQ(a->generation, 1);
	ASSERT_EQ(c->generation, 3);
}

TEST_CASE("/resolver/BNodeStore/OldestCV") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	ASSERT(!store.oldestCV().has_value());

	BNodeHeader* a = &store.allocateLeaf(1)->header;
	BNodeHeader* b = &store.allocateInternal(2)->header;
	ASSERT(store.oldestCV() == Version{1});

	// Moving the oldest node to the end exposes the next one.
	store.updateCV(a, 5);
	ASSERT(store.oldestCV() == Version{2});

	store.releaseNode(b);
	ASSERT(store.oldestCV() == Version{5});

	store.releaseNode(a);
	ASSERT(!store.oldestCV().has_value());
}

TEST_CASE("/resolver/BNodeStore/DestructorReturnsAllBlocks") {
	PagePool pool(PageSize, BlockSize);
	const std::size_t freeBefore = freeBlockCount(pool);

	{
		KeyStore keys(pool);
		BNodeStore store(pool, keys);
		BNodeHeader* a = &store.allocateLeaf(1)->header;
		store.allocateInternal(2);
		store.allocateLeaf(3);
		store.updateCV(a, 4);

		ASSERT_EQ(freeBlockCount(pool), freeBefore - 3);
	}

	ASSERT_EQ(freeBlockCount(pool), freeBefore);
}

TEST_CASE("/resolver/BNodeStore/OldestNode") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	ASSERT(store.oldestNode() == nullptr);

	BNodeHeader* a = &store.allocateLeaf(1)->header;
	BNodeHeader* b = &store.allocateInternal(2)->header;
	ASSERT(store.oldestNode() == a);

	store.updateCV(a, 5);
	ASSERT(store.oldestNode() == b);

	store.releaseNode(b);
	ASSERT(store.oldestNode() == a);

	store.releaseNode(a);
	ASSERT(store.oldestNode() == nullptr);
}

TEST_CASE("/resolver/BNodeStore/HasSpace") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	LeafBlock* leaf = store.allocateLeaf(1);
	InternalBlock* internal = store.allocateInternal(1);

	ASSERT(leaf->header.hasSpace());
	ASSERT(internal->header.hasSpace());

	// One free slot is still space.
	leaf->header.freeSlots = 1;
	internal->header.freeSlots = std::uint64_t{1} << (InternalCapacity - 1);
	ASSERT(leaf->header.hasSpace());
	ASSERT(internal->header.hasSpace());

	leaf->header.freeSlots = 0;
	internal->header.freeSlots = 0;
	ASSERT(!leaf->header.hasSpace());
	ASSERT(!internal->header.hasSpace());
}

namespace {

struct Entry {
	KeyRef key;
	BNodeHeader* child;
	Version cv;
};

bool sameKeyRef(const KeyRef& a, const KeyRef& b) {
	return a.firstBlock == b.firstBlock && a.keyOffset == b.keyOffset &&
		   a.keyLength == b.keyLength && a.blockGen == b.blockGen;
}

bool sameRangeRef(const RangeRef& a, const RangeRef& b) {
	return a.firstBlock == b.firstBlock && a.keyOffset == b.keyOffset &&
		   a.beginLength == b.beginLength && a.endStart == b.endStart &&
		   a.endLength == b.endLength && a.blockGen == b.blockGen;
}

// Stores [{'k', value}, end): a point range, or a general range ending at
// {'k', value, 1}. By default, even values give points.
RangeRef storeLeafRange(KeyStore& keys, unsigned value, Version cv,
						bool point) {
	const uint8_t begin[] = {'k', static_cast<uint8_t>(value)};
	const uint8_t pointEnd[] = {'k', static_cast<uint8_t>(value), 0};
	const uint8_t general[] = {'k', static_cast<uint8_t>(value), 1};

	const StringRef beginKey(begin, 2);
	const StringRef endKey(point ? pointEnd : general, 3);
	ASSERT(compare(beginKey, endKey) < 0);

	const std::optional<RangeRef> range = keys.appendRange(beginKey, endKey, cv);
	ASSERT(range.has_value());
	ASSERT_EQ(range->endStart, point ? 0 : 2);
	return *range;
}

RangeRef storeLeafRange(KeyStore& keys, unsigned value, Version cv) {
	return storeLeafRange(keys, value, cv, value % 2 == 0);
}

// Checks that the node holds exactly the expected entries, in order, and
// that the slots in use are the ones marked as not free.
void checkInternalNode(const InternalBlock* node,
					   const std::vector<Entry>& expected) {
	ASSERT_EQ(node->header.count, expected.size());

	std::uint64_t used = 0;

	for (std::size_t i = 0; i < expected.size(); ++i) {
		const std::size_t slot = node->header.order[i];
		ASSERT(slot < InternalCapacity);

		// Each slot appears once in the order.
		ASSERT((used & (std::uint64_t{1} << slot)) == 0);
		used |= std::uint64_t{1} << slot;

		const Entry& e = expected[i];
		ASSERT(sameKeyRef(node->separators[slot], e.key));
		ASSERT_EQ(node->separatorsCV[slot], e.cv);
		ASSERT(node->children[slot] == e.child);
		ASSERT_EQ(node->maxSubtreesCV[slot], e.child->maxCV);
		ASSERT(e.child->parent == &node->header);
		ASSERT_EQ(e.child->parentSlot, slot);
	}

	ASSERT_EQ(node->header.freeSlots, InternalSlotMask & ~used);
	ASSERT_EQ(node->header.hasSpace(), expected.size() < InternalCapacity);
}

// Fills an empty internal node, inserting entry n at rankFor(n), and checks
// the node against a reference sequence after every insertion.
template<typename RankFor>
void fillInternalNode(RankFor rankFor) {
	PagePool pool(4 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	InternalBlock* node = store.allocateInternal(1);
	ASSERT(node != nullptr);
	checkInternalNode(node, {});

	std::vector<Entry> expected;

	for (std::size_t n = 0; n < InternalCapacity; ++n) {
		const std::byte bytes[] = {
			std::byte{'k'}, static_cast<std::byte>(n)
		};
		const std::optional<KeyRef> key = keys.appendKey(bytes, 2, 1);
		ASSERT(key.has_value());

		// Children have distinct versions, different from the separator's.
		LeafBlock* child = store.allocateLeaf(static_cast<Version>(10 + n));
		ASSERT(child != nullptr);
		const Version cv = 1000 + n;

		const std::size_t rank = rankFor(n);
		ASSERT(rank <= n);

		store.insertAt(rank, *key, &child->header, cv, node);
		expected.insert(expected.begin() + rank, {*key, &child->header, cv});

		checkInternalNode(node, expected);
	}

	ASSERT(!node->header.hasSpace());
}

} // namespace

TEST_CASE("/resolver/BNodeStore/InsertAtEnd") {
	fillInternalNode([](std::size_t count) { return count; });
}

TEST_CASE("/resolver/BNodeStore/InsertAtBeginning") {
	fillInternalNode([](std::size_t) { return std::size_t{0}; });
}

TEST_CASE("/resolver/BNodeStore/InsertInMiddle") {
	fillInternalNode([](std::size_t count) { return count / 2; });
}

// Alternates beginning, end, middle and the positions next to both ends.
TEST_CASE("/resolver/BNodeStore/InsertAtMixedPositions") {
	fillInternalNode([](std::size_t count) {
		switch (count % 5) {
		case 0:
			return std::size_t{0};
		case 1:
			return count;
		case 2:
			return count / 2;
		case 3:
			return std::size_t{1};
		default:
			return count - 1;
		}
	});
}

namespace {

struct LeafEntry {
	RangeRef range;
	Version cv;
};

void checkLeafNode(const LeafBlock* node,
				   const std::vector<LeafEntry>& expected) {
	ASSERT_EQ(node->header.count, expected.size());

	std::uint64_t used = 0;

	for (std::size_t i = 0; i < expected.size(); ++i) {
		const std::size_t slot = node->header.order[i];
		ASSERT(slot < LeafCapacity);

		ASSERT((used & (std::uint64_t{1} << slot)) == 0);
		used |= std::uint64_t{1} << slot;

		ASSERT(sameRangeRef(node->ranges[slot], expected[i].range));
		ASSERT_EQ(node->cv[slot], expected[i].cv);
	}

	ASSERT_EQ(node->header.freeSlots, LeafSlotMask & ~used);
	ASSERT_EQ(node->header.hasSpace(), expected.size() < LeafCapacity);
}

// Fills an empty leaf, inserting entry n at rankFor(n), and checks the
// leaf against a reference sequence after every insertion.
template<typename RankFor>
void fillLeafNode(RankFor rankFor) {
	PagePool pool(4 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	LeafBlock* node = store.allocateLeaf(1);
	ASSERT(node != nullptr);
	checkLeafNode(node, {});

	std::vector<LeafEntry> expected;

	for (std::size_t n = 0; n < LeafCapacity; ++n) {
		const Version cv = 1000 + n;
		const RangeRef range = storeLeafRange(keys, n, cv);

		const std::size_t rank = rankFor(n);
		ASSERT(rank <= n);

		store.insertAt(rank, range, cv, node);
		expected.insert(expected.begin() + rank, {range, cv});

		checkLeafNode(node, expected);
	}

	ASSERT(!node->header.hasSpace());
}

std::size_t mixedRank(std::size_t count) {
	switch (count % 5) {
	case 0:
		return 0;
	case 1:
		return count;
	case 2:
		return count / 2;
	case 3:
		return 1;
	default:
		return count - 1;
	}
}

} // namespace

TEST_CASE("/resolver/BNodeStore/InsertAtEndOfLeaf") {
	fillLeafNode([](std::size_t count) { return count; });
}

TEST_CASE("/resolver/BNodeStore/InsertAtBeginningOfLeaf") {
	fillLeafNode([](std::size_t) { return std::size_t{0}; });
}

TEST_CASE("/resolver/BNodeStore/InsertInMiddleOfLeaf") {
	fillLeafNode([](std::size_t count) { return count / 2; });
}

TEST_CASE("/resolver/BNodeStore/InsertAtMixedPositionsOfLeaf") {
	fillLeafNode(mixedRank);
}

namespace {

// Keys {'k', 10}, {'k', 20}, ..., inserted in an order different from
// their sorted order, so that the physical slots are a permutation of the
// logical order.
constexpr std::size_t SearchKeyCount = 10;
constexpr std::size_t InsertionOrder[SearchKeyCount] = {
	5, 2, 8, 0, 9, 3, 7, 1, 6, 4
};

KeyRef storeSearchKey(KeyStore& keys, unsigned value) {
	const std::byte bytes[] = {std::byte{'k'}, static_cast<std::byte>(value)};
	const std::optional<KeyRef> key = keys.appendKey(bytes, 2, 1);
	ASSERT(key.has_value());
	return *key;
}

// Inserts the search keys through insert(rank, key) at their sorted rank.
template<typename Insert>
void insertSearchKeys(Insert insert) {
	std::vector<std::size_t> inserted;

	for (std::size_t index : InsertionOrder) {
		const auto position =
			std::lower_bound(inserted.begin(), inserted.end(), index);
		const std::size_t rank = position - inserted.begin();

		insert(rank, static_cast<unsigned>(10 * (index + 1)));
		inserted.insert(position, index);
	}
}

// Checks findKey with external keys: present keys, and absent keys
// before, between and after them.
template<typename Node>
void checkSearch(const BNodeStore& store, const Node* node) {
	std::size_t rank = 999;

	for (std::size_t i = 0; i < SearchKeyCount; ++i) {
		const uint8_t present[] = {'k', static_cast<uint8_t>(10 * (i + 1))};
		ASSERT(store.findKey(StringRef(present, 2), rank, node));
		ASSERT_EQ(rank, i);
	}

	for (std::size_t i = 0; i <= SearchKeyCount; ++i) {
		// 5 is before the first key, 105 after the last.
		const uint8_t absent[] = {'k', static_cast<uint8_t>(10 * i + 5)};
		rank = 999;
		ASSERT(!store.findKey(StringRef(absent, 2), rank, node));
		ASSERT_EQ(rank, i);
	}

	// A proper prefix of every key, and a key longer than one of them.
	const uint8_t prefix[] = {'k'};
	ASSERT(!store.findKey(StringRef(prefix, 1), rank, node));
	ASSERT_EQ(rank, 0);

	const uint8_t extended[] = {'k', 30, 0};
	ASSERT(!store.findKey(StringRef(extended, 3), rank, node));
	ASSERT_EQ(rank, 3);

	// The empty key precedes every key.
	ASSERT(!store.findKey(StringRef(), rank, node));
	ASSERT_EQ(rank, 0);
}

template<std::size_t C>
bool orderIsIdentity(const std::uint8_t (&order)[C], std::size_t count) {
	for (std::size_t i = 0; i < count; ++i) {
		if (order[i] != i) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("/resolver/BNodeStore/FindKeyInEmptyNodes") {
	PagePool pool(PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	const LeafBlock* leaf = store.allocateLeaf(1);
	const InternalBlock* internal = store.allocateInternal(1);
	const uint8_t bytes[] = {'k', 10};
	const StringRef key(bytes, 2);

	std::size_t rank = 999;
	ASSERT(!store.findKey(key, rank, leaf));
	ASSERT_EQ(rank, 0);

	rank = 999;
	ASSERT(!store.findKey(key, rank, internal));
	ASSERT_EQ(rank, 0);
}

TEST_CASE("/resolver/BNodeStore/FindKeyInLeaf") {
	PagePool pool(4 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	LeafBlock* leaf = store.allocateLeaf(1);
	insertSearchKeys([&](std::size_t rank, unsigned value) {
		store.insertAt(rank, storeLeafRange(keys, value, 1, value % 20 == 0),
					   1, leaf);
	});

	ASSERT_EQ(leaf->header.count, SearchKeyCount);
	ASSERT(!orderIsIdentity(leaf->header.order, SearchKeyCount));

	checkSearch(store, static_cast<const LeafBlock*>(leaf));
}

TEST_CASE("/resolver/BNodeStore/FindKeyInInternalNode") {
	PagePool pool(4 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	InternalBlock* internal = store.allocateInternal(1);
	insertSearchKeys([&](std::size_t rank, unsigned value) {
		LeafBlock* child = store.allocateLeaf(1);
		store.insertAt(rank, storeSearchKey(keys, value), &child->header, 1,
					   internal);
	});

	ASSERT_EQ(internal->header.count, SearchKeyCount);
	ASSERT(!orderIsIdentity(internal->header.order, SearchKeyCount));

	checkSearch(store, static_cast<const InternalBlock*>(internal));
}

namespace {

// Valid erase positions for a node holding count entries.
std::size_t mixedEraseRank(std::size_t count) {
	switch (count % 5) {
	case 0:
		return 0;
	case 1:
		return count - 1;
	case 2:
		return count / 2;
	case 3:
		return 1;
	default:
		return count - 2;
	}
}

std::uint32_t nextRandom(std::uint32_t& state) {
	state = state * 1103515245u + 12345u;
	return state >> 8;
}

// Inserts a new entry at rank and records it in expected.
void insertInternalEntry(BNodeStore& store, KeyStore& keys,
						 InternalBlock* node, std::vector<Entry>& expected,
						 std::size_t rank, std::size_t n) {
	const std::byte bytes[] = {std::byte{'k'}, static_cast<std::byte>(n)};
	const std::optional<KeyRef> key = keys.appendKey(bytes, 2, 1);
	ASSERT(key.has_value());

	LeafBlock* child = store.allocateLeaf(static_cast<Version>(10 + n));
	ASSERT(child != nullptr);
	const Version cv = 1000 + n;

	store.insertAt(rank, *key, &child->header, cv, node);
	expected.insert(expected.begin() + rank, {*key, &child->header, cv});
}

void insertLeafEntry(KeyStore& keys, BNodeStore& store, LeafBlock* node,
					 std::vector<LeafEntry>& expected, std::size_t rank,
					 std::size_t n) {
	const Version cv = 1000 + n;
	const RangeRef range = storeLeafRange(keys, n, cv);

	store.insertAt(rank, range, cv, node);
	expected.insert(expected.begin() + rank, {range, cv});
}

// Fills an internal node, then empties it erasing at rankFor(count) and
// checking the node against the reference sequence after every erase.
template<typename RankFor>
void emptyInternalNode(RankFor rankFor) {
	PagePool pool(4 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	InternalBlock* node = store.allocateInternal(1);
	std::vector<Entry> expected;

	for (std::size_t n = 0; n < InternalCapacity; ++n) {
		insertInternalEntry(store, keys, node, expected, n, n);
	}
	checkInternalNode(node, expected);

	while (!expected.empty()) {
		const std::size_t rank = rankFor(expected.size());
		ASSERT(rank < expected.size());

		store.eraseAt(rank, node);
		expected.erase(expected.begin() + rank);

		checkInternalNode(node, expected);
	}

	ASSERT_EQ(node->header.freeSlots, InternalSlotMask);
}

template<typename RankFor>
void emptyLeafNode(RankFor rankFor) {
	PagePool pool(4 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	LeafBlock* node = store.allocateLeaf(1);
	std::vector<LeafEntry> expected;

	for (std::size_t n = 0; n < LeafCapacity; ++n) {
		insertLeafEntry(keys, store, node, expected, n, n);
	}
	checkLeafNode(node, expected);

	while (!expected.empty()) {
		const std::size_t rank = rankFor(expected.size());
		ASSERT(rank < expected.size());

		store.eraseAt(rank, node);
		expected.erase(expected.begin() + rank);

		checkLeafNode(node, expected);
	}

	ASSERT_EQ(node->header.freeSlots, LeafSlotMask);
}

} // namespace

TEST_CASE("/resolver/BNodeStore/EraseAtEnd") {
	emptyInternalNode([](std::size_t count) { return count - 1; });
}

TEST_CASE("/resolver/BNodeStore/EraseAtBeginning") {
	emptyInternalNode([](std::size_t) { return std::size_t{0}; });
}

TEST_CASE("/resolver/BNodeStore/EraseInMiddle") {
	emptyInternalNode([](std::size_t count) { return count / 2; });
}

TEST_CASE("/resolver/BNodeStore/EraseAtMixedPositions") {
	emptyInternalNode(mixedEraseRank);
}

TEST_CASE("/resolver/BNodeStore/EraseAtEndOfLeaf") {
	emptyLeafNode([](std::size_t count) { return count - 1; });
}

TEST_CASE("/resolver/BNodeStore/EraseAtBeginningOfLeaf") {
	emptyLeafNode([](std::size_t) { return std::size_t{0}; });
}

TEST_CASE("/resolver/BNodeStore/EraseInMiddleOfLeaf") {
	emptyLeafNode([](std::size_t count) { return count / 2; });
}

TEST_CASE("/resolver/BNodeStore/EraseAtMixedPositionsOfLeaf") {
	emptyLeafNode(mixedEraseRank);
}

// Random inserts and erases: every insert takes the lowest free slot, so
// slots freed by erases are reused.
TEST_CASE("/resolver/BNodeStore/InsertAndEraseInterleaved") {
	PagePool pool(16 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	InternalBlock* node = store.allocateInternal(1);
	std::vector<Entry> expected;
	std::uint32_t state = 4242;

	for (std::size_t n = 0; n < 300; ++n) {
		const std::size_t count = expected.size();
		const bool insert = count == 0 ||
			(count < InternalCapacity && nextRandom(state) % 3 != 0);

		if (insert) {
			const unsigned lowestFree = std::countr_zero(node->header.freeSlots);
			const std::size_t rank = nextRandom(state) % (count + 1);

			insertInternalEntry(store, keys, node, expected, rank, n);
			ASSERT_EQ(node->header.order[rank], lowestFree);
		} else {
			const std::size_t rank = nextRandom(state) % count;

			store.eraseAt(rank, node);
			expected.erase(expected.begin() + rank);
		}

		checkInternalNode(node, expected);
	}
}

TEST_CASE("/resolver/BNodeStore/InsertAndEraseInterleavedInLeaf") {
	PagePool pool(4 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	LeafBlock* node = store.allocateLeaf(1);
	std::vector<LeafEntry> expected;
	std::uint32_t state = 777;

	for (std::size_t n = 0; n < 300; ++n) {
		const std::size_t count = expected.size();
		const bool insert = count == 0 ||
			(count < LeafCapacity && nextRandom(state) % 3 != 0);

		if (insert) {
			const unsigned lowestFree = std::countr_zero(node->header.freeSlots);
			const std::size_t rank = nextRandom(state) % (count + 1);

			insertLeafEntry(keys, store, node, expected, rank, n);
			ASSERT_EQ(node->header.order[rank], lowestFree);
		} else {
			const std::size_t rank = nextRandom(state) % count;

			store.eraseAt(rank, node);
			expected.erase(expected.begin() + rank);
		}

		checkLeafNode(node, expected);
	}
}

// In a full node, erasing one entry frees exactly its slot, and the next
// insertion, at another position, reuses it.
TEST_CASE("/resolver/BNodeStore/EraseThenInsertReusesSlot") {
	PagePool pool(4 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	InternalBlock* node = store.allocateInternal(1);
	std::vector<Entry> expected;

	for (std::size_t n = 0; n < InternalCapacity; ++n) {
		insertInternalEntry(store, keys, node, expected, 0, n);
	}
	ASSERT(!node->header.hasSpace());

	const std::size_t erased = InternalCapacity / 2;
	const unsigned slot = node->header.order[erased];

	store.eraseAt(erased, node);
	expected.erase(expected.begin() + erased);
	checkInternalNode(node, expected);
	ASSERT_EQ(node->header.freeSlots, std::uint64_t{1} << slot);

	insertInternalEntry(store, keys, node, expected, 3, InternalCapacity);
	ASSERT_EQ(node->header.order[3], slot);
	checkInternalNode(node, expected);
	ASSERT(!node->header.hasSpace());
}

TEST_CASE("/resolver/BNodeStore/EraseThenInsertReusesSlotInLeaf") {
	PagePool pool(4 * PageSize, BlockSize);
	KeyStore keys(pool);
	BNodeStore store(pool, keys);

	LeafBlock* node = store.allocateLeaf(1);
	std::vector<LeafEntry> expected;

	for (std::size_t n = 0; n < LeafCapacity; ++n) {
		insertLeafEntry(keys, store, node, expected, 0, n);
	}
	ASSERT(!node->header.hasSpace());

	const std::size_t erased = LeafCapacity / 2;
	const unsigned slot = node->header.order[erased];

	store.eraseAt(erased, node);
	expected.erase(expected.begin() + erased);
	checkLeafNode(node, expected);
	ASSERT_EQ(node->header.freeSlots, std::uint64_t{1} << slot);

	insertLeafEntry(keys, store, node, expected, 3, LeafCapacity);
	ASSERT_EQ(node->header.order[3], slot);
	checkLeafNode(node, expected);
	ASSERT(!node->header.hasSpace());
}
