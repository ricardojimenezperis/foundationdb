#include "test.h"
#include "tempconflictbtree.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Gives the tests access to the private parts of the tree.
struct TempConflictBTreeTestAccess {
	static KeyStore& keys(TempConflictBTree& t) { return t.keyStore_; }
	static BNodeStore& nodes(TempConflictBTree& t) { return t.nodeStore_; }
	static BNodeHeader*& root(TempConflictBTree& t) { return t.root_; }

	static bool findKey(TempConflictBTree& t, const StringRef& key,
						LeafBlock*& leaf, std::size_t& rank,
						BNodeHeader*& nodeNeedsSplit) {
		return t.findKey(key, leaf, rank, nodeNeedsSplit);
	}
	static bool nextRank(const TempConflictBTree& t, LeafBlock*& leaf,
						 std::size_t& rank, std::size_t& slot) {
		return t.nextRank(leaf, rank, slot);
	}
	static bool prevRank(const TempConflictBTree& t, LeafBlock*& leaf,
						 std::size_t& rank, std::size_t& slot) {
		return t.prevRank(leaf, rank, slot);
	}
	static bool hasConflict(TempConflictBTree& t,
							const ReadConflictRange& read) {
		return t.hasConflict(read);
	}
	static void setReclaimCV(TempConflictBTree& t, Version cv) {
		t.reclaimCV_ = cv;
	}
	static bool refreshSeparator(TempConflictBTree& t, InternalBlock* node,
								 std::size_t rank) {
		return t.refreshSeparator(node, rank);
	}
	static bool refreshSeparators(TempConflictBTree& t, InternalBlock* node) {
		return t.refreshSeparators(node);
	}
	static bool cleanExpired(TempConflictBTree& t, LeafBlock* leaf) {
		return t.cleanExpired(leaf);
	}
	static void split(TempConflictBTree& t, BNodeHeader* node,
					  LeafBlock*& leaf, std::size_t& rank, bool found,
					  Version cv) {
		t.split(node, leaf, rank, found, cv);
	}
	static bool needsSplit(const BNodeHeader* node) {
		return TempConflictBTree::needsSplit(node);
	}
	static bool needsSplit(const InternalBlock* node) {
		return TempConflictBTree::needsSplit(node);
	}
	static bool needsSplit(const LeafBlock* node) {
		return TempConflictBTree::needsSplit(node);
	}
};

namespace {

using Access = TempConflictBTreeTestAccess;
using Bytes = std::vector<uint8_t>;

StringRef asRef(const Bytes& bytes) {
	return StringRef(bytes.data(), static_cast<int>(bytes.size()));
}

struct Entry {
	Bytes begin;
	Bytes end;
	Version cv;
};

struct LeafModel {
	LeafBlock* leaf;
	Bytes lowerBound;               // empty for the first leaf
	std::vector<Entry> entries;     // in key order
};

// A leaf needs a split when it is full; an internal node when every free
// slot may be taken by the split of a child that needs one.
bool needsSplit(const BNodeHeader* node) {
	if (node->kind == BNodeKind::Leaf) {
		return !node->hasSpace();
	}
	return node->childrenNeedingSplit == node->freeSlotCount();
}

std::uint8_t childrenNeedingSplit(const InternalBlock* node) {
	std::uint8_t count = 0;
	for (std::size_t r = 0; r < node->header.count; ++r) {
		count += needsSplit(node->children[node->header.order[r]]);
	}
	return count;
}

// Separator CV given to empty leaves by TestTree::build.
constexpr Version EmptyLeafCV = 1000;

// A tree built directly from nodes. Entries are inserted in reverse order,
// always at rank 0, so that order is never the identity.
class TestTree {
public:
	explicit TestTree(std::size_t budget = 16 * PageSize) : tree(budget) {}

	TempConflictBTree tree;
	std::vector<LeafModel> leaves;

	KeyStore& keys() { return Access::keys(tree); }
	BNodeStore& nodes() { return Access::nodes(tree); }

	LeafBlock* addLeaf(const Bytes& lowerBound, std::vector<Entry> entries) {
		LeafBlock* leaf = nodes().allocateLeaf(0);
		ASSERT(leaf != nullptr);

		for (auto e = entries.rbegin(); e != entries.rend(); ++e) {
			const std::optional<RangeRef> range =
				keys().appendRange(asRef(e->begin), asRef(e->end), e->cv);
			ASSERT(range.has_value());
			nodes().insertAt(0, *range, e->cv, leaf);
		}

		if (!leaves.empty()) {
			leaves.back().leaf->right = leaf;
			leaf->left = leaves.back().leaf;
		}
		leaves.push_back({leaf, lowerBound, std::move(entries)});
		return leaf;
	}

	// Builds internal levels over the leaves, fanout children per node,
	// with each separator the lower bound of the first leaf below it.
	void build(std::size_t fanout) {
		struct Child {
			BNodeHeader* node;
			Bytes lowerBound;
			// CV of the first entry below, as refreshing sets it. An empty
			// leaf gets EmptyLeafCV, newer than any entry, as if its entries
			// had been overwritten after its separator was set.
			Version cv;
		};
		std::vector<Child> level;
		for (const LeafModel& l : leaves) {
			level.push_back({&l.leaf->header, l.lowerBound,
							 l.entries.empty() ? EmptyLeafCV : l.entries.front().cv});
		}

		while (level.size() > 1) {
			std::vector<Child> parents;
			for (std::size_t i = 0; i < level.size(); i += fanout) {
				InternalBlock* internal = nodes().allocateInternal(0);
				ASSERT(internal != nullptr);
				const std::size_t last = std::min(level.size(), i + fanout);
				for (std::size_t j = last; j > i; --j) {
					const Bytes& separator = level[j - 1].lowerBound;
					const std::optional<KeyRef> key = keys().appendKey(
						reinterpret_cast<const std::byte*>(separator.data()),
						static_cast<std::uint16_t>(separator.size()), 0);
					ASSERT(key.has_value());
					nodes().insertAt(0, *key, level[j - 1].node, level[j - 1].cv,
									 internal);
				}
				internal->header.childrenNeedingSplit =
					childrenNeedingSplit(internal);
				parents.push_back({&internal->header, level[i].lowerBound,
								   level[i].cv});
			}
			level = std::move(parents);
		}

		Access::root(tree) = level.empty() ? nullptr : level.front().node;
	}

	// The leaf a search for key must reach, and its position there.
	std::size_t expectedLeaf(const Bytes& key) const {
		std::size_t index = 0;
		for (std::size_t i = 1; i < leaves.size(); ++i) {
			if (!(key < leaves[i].lowerBound)) {
				index = i;
			}
		}
		return index;
	}

	std::size_t expectedRank(std::size_t leaf, const Bytes& key,
							 bool& found) const {
		const std::vector<Entry>& entries = leaves[leaf].entries;
		std::size_t rank = 0;
		while (rank < entries.size() && entries[rank].begin < key) {
			++rank;
		}
		found = rank < entries.size() && entries[rank].begin == key;
		return rank;
	}

	// Whether a read overlaps a stored range with a newer CV.
	bool expectedConflict(const Bytes& begin, const Bytes& end,
						  Version version) const {
		for (const LeafModel& l : leaves) {
			for (const Entry& e : l.entries) {
				if (e.begin < end && begin < e.end && e.cv > version) {
					return true;
				}
			}
		}
		return false;
	}

	bool hasConflict(const Bytes& begin, const Bytes& end, Version version) {
		return Access::hasConflict(
			tree, ReadConflictRange(asRef(begin), asRef(end), version, 0, 0));
	}
};

Bytes key(unsigned value) {
	return Bytes{'k', static_cast<uint8_t>(value)};
}

Bytes keyAfter(Bytes k) {
	k.push_back(0);
	return k;
}

// Entries [k(first), ...) with even keys: points on multiples of 4 and
// general ranges [k(v), k(v + 1)) otherwise, all disjoint.
std::vector<Entry> entriesFrom(unsigned first, unsigned count, Version cv) {
	std::vector<Entry> entries;
	for (unsigned i = 0; i < count; ++i) {
		const unsigned v = first + 2 * i;
		entries.push_back({key(v), v % 4 == 0 ? keyAfter(key(v)) : key(v + 1),
						   cv + static_cast<Version>(i)});
	}
	return entries;
}

void checkFindKey(TestTree& t, const Bytes& k) {
	LeafBlock* leaf = nullptr;
	std::size_t rank = 999;
	BNodeHeader* nodeNeedsSplit = nullptr;
	const bool found = Access::findKey(t.tree, asRef(k), leaf, rank, nodeNeedsSplit);

	const std::size_t expected = t.expectedLeaf(k);
	bool expectedFound = false;
	const std::size_t expectedRank = t.expectedRank(expected, k, expectedFound);

	ASSERT(leaf == t.leaves[expected].leaf);
	ASSERT_EQ(rank, expectedRank);
	ASSERT_EQ(found, expectedFound);
}

// Every key from k(0) to k(lastValue), each key followed by zero, and a
// prefix shorter than every key.
std::vector<Bytes> searchKeys(unsigned lastValue) {
	std::vector<Bytes> keys{Bytes{}, Bytes{'k'}};
	for (unsigned v = 0; v <= lastValue; ++v) {
		keys.push_back(key(v));
		keys.push_back(keyAfter(key(v)));
	}
	keys.push_back(Bytes{'l'});
	return keys;
}

} // namespace

TEST_CASE("/resolver/TempConflictBTree/FindKeyInEmptyTree") {
	TestTree t;

	LeafBlock* leaf = reinterpret_cast<LeafBlock*>(&t);
	std::size_t rank = 999;
	BNodeHeader* nodeNeedsSplit = nullptr;
	ASSERT(!Access::findKey(t.tree, asRef(key(1)), leaf, rank, nodeNeedsSplit));
	ASSERT(leaf == nullptr);
	ASSERT_EQ(rank, 0);
}

TEST_CASE("/resolver/TempConflictBTree/FindKeyInSingleLeaf") {
	TestTree t;
	t.addLeaf({}, entriesFrom(10, 6, 1));
	t.build(3);
	ASSERT(Access::root(t.tree) == &t.leaves[0].leaf->header);

	for (const Bytes& k : searchKeys(30)) {
		checkFindKey(t, k);
	}
}

TEST_CASE("/resolver/TempConflictBTree/FindKeyThroughOneInternalLevel") {
	TestTree t;
	t.addLeaf({}, entriesFrom(10, 4, 1));
	t.addLeaf(key(20), entriesFrom(20, 5, 1));
	t.addLeaf(key(40), entriesFrom(40, 3, 1));
	t.build(3);
	ASSERT(Access::root(t.tree)->kind == BNodeKind::Internal);

	for (const Bytes& k : searchKeys(60)) {
		checkFindKey(t, k);
	}
}

TEST_CASE("/resolver/TempConflictBTree/FindKeyThroughTwoInternalLevels") {
	TestTree t;
	for (unsigned i = 0; i < 7; ++i) {
		const unsigned first = 10 + 20 * i;
		t.addLeaf(i == 0 ? Bytes{} : key(first), entriesFrom(first, 4, 1));
	}
	t.build(2);

	const auto* root = reinterpret_cast<const InternalBlock*>(Access::root(t.tree));
	ASSERT(root->header.kind == BNodeKind::Internal);
	ASSERT(root->children[root->header.order[0]]->kind == BNodeKind::Internal);

	for (const Bytes& k : searchKeys(160)) {
		checkFindKey(t, k);
	}
}

// A leaf without entries is reached for the keys between its neighbours.
TEST_CASE("/resolver/TempConflictBTree/FindKeyReachesEmptyLeaf") {
	TestTree t;
	t.addLeaf({}, entriesFrom(10, 3, 1));
	t.addLeaf(key(20), {});
	t.addLeaf(key(30), entriesFrom(30, 3, 1));
	t.build(3);

	for (const Bytes& k : searchKeys(50)) {
		checkFindKey(t, k);
	}

	LeafBlock* leaf = nullptr;
	std::size_t rank = 999;
	BNodeHeader* nodeNeedsSplit = nullptr;
	ASSERT(!Access::findKey(t.tree, asRef(key(25)), leaf, rank, nodeNeedsSplit));
	ASSERT(leaf == t.leaves[1].leaf);
	ASSERT_EQ(rank, 0);
}

namespace {

// Leaves with 3, 0, 2, 0, 0 and 4 entries, so that moving between entries
// crosses one and two empty leaves.
TestTree& leavesWithGaps(TestTree& t) {
	t.addLeaf({}, entriesFrom(10, 3, 1));
	t.addLeaf(key(20), {});
	t.addLeaf(key(30), entriesFrom(30, 2, 1));
	t.addLeaf(key(40), {});
	t.addLeaf(key(50), {});
	t.addLeaf(key(60), entriesFrom(60, 4, 1));
	t.build(3);
	return t;
}

// All entries in key order, as (leaf, rank).
std::vector<std::pair<LeafBlock*, std::size_t>> allPositions(const TestTree& t) {
	std::vector<std::pair<LeafBlock*, std::size_t>> positions;
	for (const LeafModel& l : t.leaves) {
		for (std::size_t r = 0; r < l.entries.size(); ++r) {
			positions.push_back({l.leaf, r});
		}
	}
	return positions;
}

} // namespace

TEST_CASE("/resolver/TempConflictBTree/NextRankVisitsEveryEntry") {
	TestTree t;
	leavesWithGaps(t);
	const auto positions = allPositions(t);

	LeafBlock* leaf = t.leaves[0].leaf;
	std::size_t rank = 0;
	std::size_t slot = 999;

	for (std::size_t i = 1; i < positions.size(); ++i) {
		ASSERT(Access::nextRank(t.tree, leaf, rank, slot));
		ASSERT(leaf == positions[i].first);
		ASSERT_EQ(rank, positions[i].second);
		ASSERT_EQ(slot, leaf->header.order[rank]);
	}

	// At the last entry: no next one, rank becomes the end position, and
	// leaf and slot do not change.
	const std::size_t lastSlot = slot;
	ASSERT(!Access::nextRank(t.tree, leaf, rank, slot));
	ASSERT(leaf == t.leaves.back().leaf);
	ASSERT_EQ(rank, leaf->header.count);
	ASSERT_EQ(slot, lastSlot);
}

TEST_CASE("/resolver/TempConflictBTree/PrevRankVisitsEveryEntry") {
	TestTree t;
	leavesWithGaps(t);
	const auto positions = allPositions(t);

	// From the end position of the last leaf.
	LeafBlock* leaf = t.leaves.back().leaf;
	std::size_t rank = leaf->header.count;
	std::size_t slot = 999;

	for (std::size_t i = positions.size(); i > 0; --i) {
		ASSERT(Access::prevRank(t.tree, leaf, rank, slot));
		ASSERT(leaf == positions[i - 1].first);
		ASSERT_EQ(rank, positions[i - 1].second);
		ASSERT_EQ(slot, leaf->header.order[rank]);
	}

	// At the first entry: no previous one, and nothing changes.
	const std::size_t firstSlot = slot;
	ASSERT(!Access::prevRank(t.tree, leaf, rank, slot));
	ASSERT(leaf == t.leaves.front().leaf);
	ASSERT_EQ(rank, 0);
	ASSERT_EQ(slot, firstSlot);
}

// From the position of an empty leaf, both directions skip to the nearest
// entries; from the end position of a leaf, nextRank goes right.
TEST_CASE("/resolver/TempConflictBTree/RankMovesFromEmptyLeaves") {
	TestTree t;
	leavesWithGaps(t);
	std::size_t slot = 999;

	LeafBlock* leaf = t.leaves[3].leaf;
	std::size_t rank = 0;
	ASSERT(Access::prevRank(t.tree, leaf, rank, slot));
	ASSERT(leaf == t.leaves[2].leaf);
	ASSERT_EQ(rank, 1);
	ASSERT_EQ(slot, leaf->header.order[1]);

	leaf = t.leaves[3].leaf;
	rank = 0;
	ASSERT(Access::nextRank(t.tree, leaf, rank, slot));
	ASSERT(leaf == t.leaves[5].leaf);
	ASSERT_EQ(rank, 0);
	ASSERT_EQ(slot, leaf->header.order[0]);

	leaf = t.leaves[0].leaf;
	rank = leaf->header.count;
	ASSERT(Access::nextRank(t.tree, leaf, rank, slot));
	ASSERT(leaf == t.leaves[2].leaf);
	ASSERT_EQ(rank, 0);

	// A single empty leaf has no entries in either direction.
	TestTree single;
	single.addLeaf({}, {});
	single.build(3);
	leaf = single.leaves[0].leaf;
	rank = 0;
	slot = 7;
	ASSERT(!Access::prevRank(single.tree, leaf, rank, slot));
	ASSERT(!Access::nextRank(single.tree, leaf, rank, slot));
	ASSERT(leaf == single.leaves[0].leaf);
	ASSERT_EQ(rank, 0);
	ASSERT_EQ(slot, 7);
}

TEST_CASE("/resolver/TempConflictBTree/HasConflictInEmptyTree") {
	TestTree t;
	ASSERT(!t.hasConflict(key(1), key(9), 0));
}

// Reads around a single stored range [k(20), k(30)) with CV 10.
TEST_CASE("/resolver/TempConflictBTree/HasConflictAroundOneRange") {
	TestTree t;
	t.addLeaf({}, {{key(20), key(30), 10}});
	t.build(3);

	// Overlapping reads conflict only when the range is newer.
	ASSERT(t.hasConflict(key(20), key(21), 9));
	ASSERT(t.hasConflict(key(15), key(21), 9));
	ASSERT(t.hasConflict(key(25), key(26), 9));
	ASSERT(t.hasConflict(key(29), key(40), 9));
	ASSERT(!t.hasConflict(key(25), key(26), 10));
	ASSERT(!t.hasConflict(key(25), key(26), 11));

	// Reads touching the range without overlapping it: ranges are half-open.
	ASSERT(!t.hasConflict(key(10), key(20), 0));
	ASSERT(!t.hasConflict(key(30), key(40), 0));
	ASSERT(!t.hasConflict(key(10), key(15), 0));
	ASSERT(!t.hasConflict(key(35), key(40), 0));
}

// A point range [k, k\0) conflicts only with reads containing k.
TEST_CASE("/resolver/TempConflictBTree/HasConflictWithPoint") {
	TestTree t;
	t.addLeaf({}, {{key(20), keyAfter(key(20)), 10}});
	t.build(3);

	ASSERT(t.hasConflict(key(20), keyAfter(key(20)), 0));
	ASSERT(t.hasConflict(key(10), key(21), 0));
	ASSERT(!t.hasConflict(keyAfter(key(20)), key(30), 0));
	ASSERT(!t.hasConflict(key(10), key(20), 0));
}

// Reads checked against a brute-force scan of the stored ranges, on trees
// with several levels and empty leaves.
TEST_CASE("/resolver/TempConflictBTree/HasConflictAgainstReference") {
	for (std::size_t fanout : {std::size_t{2}, std::size_t{3}}) {
		TestTree t;
		std::uint32_t state = 4321 + static_cast<std::uint32_t>(fanout);
		auto next = [&state]() {
			state = state * 1103515245u + 12345u;
			return state >> 8;
		};

		// Ten leaves over k(10)..k(209), some of them empty, with random CVs.
		// With fanout 3 the first leaf is empty too.
		for (unsigned i = 0; i < 10; ++i) {
			const unsigned first = 10 + 20 * i;
			std::vector<Entry> entries;
			if (i % 4 != 2 && !(i == 0 && fanout == 3)) {
				entries = entriesFrom(first, 1 + next() % 6, 0);
				for (Entry& e : entries) {
					e.cv = static_cast<Version>(next() % 50);
				}
			}
			t.addLeaf(i == 0 ? Bytes{} : key(first), std::move(entries));
		}
		t.build(fanout);

		const std::vector<Bytes> keys = searchKeys(220);
		int conflicts = 0;
		int checks = 0;
		for (std::size_t b = 0; b < keys.size(); ++b) {
			for (std::size_t e = b + 1; e < keys.size(); e += 1 + next() % 4) {
				const Version version = static_cast<Version>(next() % 50);
				const bool expected =
					t.expectedConflict(keys[b], keys[e], version);
				ASSERT_EQ(t.hasConflict(keys[b], keys[e], version), expected);
				conflicts += expected;
				++checks;
			}
		}
		ASSERT(conflicts > 0 && conflicts < checks);
	}
}

// A first separator that is not the smallest key: keys before it still go
// down the first child.
TEST_CASE("/resolver/TempConflictBTree/FindKeyBeforeFirstSeparator") {
	TestTree t;
	t.addLeaf(key(10), entriesFrom(10, 3, 1));
	t.addLeaf(key(30), entriesFrom(30, 3, 1));
	t.addLeaf(key(50), entriesFrom(50, 3, 1));
	t.addLeaf(key(70), entriesFrom(70, 3, 1));
	t.build(2);

	for (const Bytes& k : searchKeys(90)) {
		checkFindKey(t, k);
	}

	LeafBlock* leaf = nullptr;
	std::size_t rank = 999;
	BNodeHeader* nodeNeedsSplit = nullptr;
	ASSERT(!Access::findKey(t.tree, asRef(key(5)), leaf, rank, nodeNeedsSplit));
	ASSERT(leaf == t.leaves[0].leaf);
	ASSERT_EQ(rank, 0);
}

// The search lands in an empty first leaf: there is no previous entry and
// no candidate in that leaf, so the ranges to the right are checked.
TEST_CASE("/resolver/TempConflictBTree/HasConflictFromEmptyFirstLeaf") {
	TestTree t;
	t.addLeaf({}, {});
	t.addLeaf(key(20), {{key(20), key(30), 10}});
	t.build(3);

	ASSERT(t.hasConflict(key(10), key(25), 9));
	ASSERT(!t.hasConflict(key(10), key(25), 10));
	ASSERT(!t.hasConflict(key(10), key(20), 0));
}

TEST_CASE("/resolver/TempConflictBTree/NewTreeHasNoReclaimedHistory") {
	TestTree t;
	ASSERT_EQ(t.tree.reclaimCV(), 0);
}

namespace {

// Reads length bytes from logical position start of a stored record.
Bytes readRecord(const KeyStore& store, const RangeRef& r, std::size_t start,
				 std::size_t length) {
	Bytes bytes;
	const KeyBlockHeader* block = r.firstBlock;
	std::size_t offset = r.keyOffset;
	for (std::size_t position = 0; position < start + length; ++position) {
		if (offset == store.blockSize()) {
			block = block->nextBlock;
			offset = sizeof(KeyBlockHeader);
		}
		if (position >= start) {
			bytes.push_back(std::to_integer<uint8_t>(
				reinterpret_cast<const std::byte*>(block)[offset]));
		}
		++offset;
	}
	return bytes;
}

// The entries of a leaf, read back from the store, in key order.
std::vector<Entry> leafEntries(TestTree& t, const LeafBlock* leaf) {
	std::vector<Entry> entries;
	for (std::size_t r = 0; r < leaf->header.count; ++r) {
		const auto slot = leaf->header.order[r];
		const RangeRef& range = leaf->ranges[slot];
		entries.push_back({readRecord(t.keys(), range, 0, range.beginLength),
						   readRecord(t.keys(), range, range.endStart,
									  range.endLength),
						   leaf->cv[slot]});
	}
	return entries;
}

// Checks the subtree under node and appends its leaves in key order.
void checkSubtree(TestTree& t, const BNodeHeader* node,
				  std::vector<const LeafBlock*>& leaves,
				  std::vector<Entry>& entries) {
	if (node->kind == BNodeKind::Leaf) {
		const auto* leaf = reinterpret_cast<const LeafBlock*>(node);
		leaves.push_back(leaf);
		for (const Entry& e : leafEntries(t, leaf)) {
			ASSERT(entries.empty() || !(e.begin < entries.back().end));
			entries.push_back(e);
		}
		return;
	}

	const auto* internal = reinterpret_cast<const InternalBlock*>(node);
	ASSERT_EQ(internal->header.childrenNeedingSplit,
			  childrenNeedingSplit(internal));
	for (std::size_t r = 0; r < internal->header.count; ++r) {
		const auto slot = internal->header.order[r];
		ASSERT(internal->children[slot]->parent == node);
		ASSERT_EQ(internal->children[slot]->parentSlot, slot);
		ASSERT(internal->maxSubtreesCV[slot] >= internal->children[slot]->maxCV);
		ASSERT(node->maxCV >= internal->children[slot]->maxCV);
		const std::size_t before = entries.size();
		checkSubtree(t, internal->children[slot], leaves, entries);

		// Every begin in this child is at least its separator, and every
		// begin in the children before it is below it.
		const KeyRef& separator = internal->separators[slot];
		for (std::size_t i = 0; i < entries.size(); ++i) {
			const int c = t.keys().compareKeys(asRef(entries[i].begin), separator);
			if (r > 0 && i < before) {
				ASSERT(c < 0);
			} else if (i >= before) {
				ASSERT(r == 0 || c >= 0);
			}
		}
	}
}

// Checks the whole tree: structure, leaf links and reads against a
// brute-force scan of the entries left in it.
std::vector<Entry> checkTree(TestTree& t) {
	std::vector<const LeafBlock*> leaves;
	std::vector<Entry> entries;
	BNodeHeader* root = Access::root(t.tree);
	if (root != nullptr) {
		checkSubtree(t, root, leaves, entries);
	}

	for (std::size_t i = 0; i < leaves.size(); ++i) {
		ASSERT(leaves[i]->left == (i == 0 ? nullptr : leaves[i - 1]));
		ASSERT(leaves[i]->right ==
			   (i + 1 == leaves.size() ? nullptr : leaves[i + 1]));
	}

	for (unsigned b = 0; b <= 230; b += 3) {
		for (unsigned e = b + 1; e <= 231; e += 7) {
			// Reads are admitted only at or above reclaimCV, and may clean
			// the tree, which never changes their outcome.
			for (Version version : {t.tree.reclaimCV(), Version{0},
									Version{15}, Version{40}}) {
				if (version < t.tree.reclaimCV()) {
					continue;
				}
				bool expected = false;
				for (const Entry& entry : entries) {
					expected |= entry.begin < key(e) && key(b) < entry.end &&
						entry.cv > version;
				}
				ASSERT_EQ(t.hasConflict(key(b), key(e), version), expected);
			}
		}
	}
	return entries;
}

std::vector<Entry> withCVs(std::vector<Entry> entries,
						   std::initializer_list<Version> cvs) {
	ASSERT_EQ(entries.size(), cvs.size());
	auto cv = cvs.begin();
	for (Entry& e : entries) {
		e.cv = *cv++;
	}
	return entries;
}

InternalBlock* rootNode(TestTree& t) {
	BNodeHeader* root = Access::root(t.tree);
	ASSERT(root != nullptr && root->kind == BNodeKind::Internal);
	return reinterpret_cast<InternalBlock*>(root);
}

BNodeHeader* childAt(const InternalBlock* node, std::size_t rank) {
	return node->children[node->header.order[rank]];
}

} // namespace

TEST_CASE("/resolver/TempConflictBTree/CleanExpiredRemovesOnlyExpiredEntries") {
	TestTree t;
	LeafBlock* leaf = t.addLeaf({}, withCVs(entriesFrom(10, 5, 0), {5, 15, 3, 20, 10}));
	t.build(3);

	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::cleanExpired(t.tree, leaf));

	const std::vector<Entry> left = leafEntries(t, leaf);
	ASSERT_EQ(left.size(), 2);
	ASSERT(left[0].begin == key(12) && left[0].cv == 15);
	ASSERT(left[1].begin == key(16) && left[1].cv == 20);
	ASSERT_EQ(leaf->header.freeSlots, LeafSlotMask & ~((std::uint64_t{1} << leaf->header.order[0]) |
											   (std::uint64_t{1} << leaf->header.order[1])));

	ASSERT(!Access::cleanExpired(t.tree, leaf));
	checkTree(t);
}

TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorKeepsFreshSeparator") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {5, 6, 7}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {8, 2, 9}));
	t.build(3);
	InternalBlock* root = rootNode(t);
	const KeyRef before = root->separators[root->header.order[1]];

	Access::setReclaimCV(t.tree, 7);
	ASSERT(!Access::refreshSeparator(t.tree, root, 1));
	ASSERT(root->separators[root->header.order[1]].firstBlock == before.firstBlock);
	ASSERT_EQ(leafEntries(t, t.leaves[1].leaf).size(), 3);
	checkTree(t);
}

// The expired separator of a leaf becomes the begin of its first entry left
// after removing its expired entries.
TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorFromLeaf") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {20, 20, 20}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 4, 0), {5, 10, 30, 8}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 3, 0), {20, 20, 20}));
	t.build(3);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparator(t.tree, root, 1));

	const auto slot = root->header.order[1];
	ASSERT_EQ(root->header.count, 3);
	ASSERT_EQ(root->separatorsCV[slot], 30);
	ASSERT_EQ(t.keys().compareKeys(asRef(key(34)), root->separators[slot]), 0);
	ASSERT(t.keys().isValid(root->separators[slot]));
	ASSERT_EQ(leafEntries(t, t.leaves[1].leaf).size(), 1);
	checkTree(t);
}

// A full leaf left empty is removed, its neighbours are linked to each other
// and the parent no longer counts it as a child needing a split.
TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorRemovesEmptyLeaf") {
	TestTree t;
	t.addLeaf({}, entriesFrom(10, 3, 100));
	t.addLeaf(key(20), entriesFrom(20, LeafCapacity, 1));
	t.addLeaf(key(160), entriesFrom(160, 3, 100));
	t.build(3);
	InternalBlock* root = rootNode(t);
	ASSERT(!t.leaves[1].leaf->header.hasSpace());
	ASSERT_EQ(root->header.childrenNeedingSplit, 1);

	Access::setReclaimCV(t.tree, LeafCapacity);
	ASSERT(Access::refreshSeparator(t.tree, root, 1));

	ASSERT_EQ(root->header.count, 2);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	ASSERT(childAt(root, 1) == &t.leaves[2].leaf->header);
	ASSERT(t.leaves[0].leaf->right == t.leaves[2].leaf);
	ASSERT(t.leaves[2].leaf->left == t.leaves[0].leaf);
	checkTree(t);
}

namespace {

// root -> [I0, I1]. I0 has a single free slot and its second child, a leaf,
// is full with CVs 1..LeafCapacity, so I0 needs a split and the root counts
// it. The other leaves hold one entry each, with CV 100.
InternalBlock* fullLeafUnderCriticalNode(TestTree& t) {
	constexpr std::size_t children = InternalCapacity - 1;
	t.addLeaf({}, entriesFrom(10, 1, 100));
	t.addLeaf(key(12), entriesFrom(12, LeafCapacity, 1));
	unsigned first = 12 + 2 * LeafCapacity;
	for (std::size_t i = 2; i <= children; ++i, first += 2) {
		t.addLeaf(key(first), entriesFrom(first, 1, 100));
	}
	ASSERT(first <= 231);
	t.build(children);

	InternalBlock* root = rootNode(t);
	ASSERT_EQ(root->header.count, 2);
	auto* i0 = reinterpret_cast<InternalBlock*>(childAt(root, 0));
	ASSERT_EQ(i0->header.count, children);
	ASSERT_EQ(i0->header.childrenNeedingSplit, 1);
	ASSERT(needsSplit(&i0->header));
	ASSERT_EQ(root->header.childrenNeedingSplit, 1);
	ASSERT(!needsSplit(&root->header));
	return i0;
}

} // namespace

// When the full leaf is cleaned it stops needing a split, and so does its
// parent, which has the leaf's slot free again: both counts go down.
TEST_CASE("/resolver/TempConflictBTree/CleanExpiredPropagatesSplitCleared") {
	TestTree t;
	InternalBlock* i0 = fullLeafUnderCriticalNode(t);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 1);
	ASSERT(Access::cleanExpired(t.tree, t.leaves[1].leaf));

	ASSERT(t.leaves[1].leaf->header.hasSpace());
	ASSERT_EQ(i0->header.childrenNeedingSplit, 0);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	checkTree(t);
}

// Cleaning a leaf that was not full leaves the counts unchanged.
TEST_CASE("/resolver/TempConflictBTree/CleanExpiredKeepsSplitCountsOfNonFullLeaf") {
	TestTree t;
	InternalBlock* i0 = fullLeafUnderCriticalNode(t);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 100);
	ASSERT(Access::cleanExpired(t.tree, t.leaves[2].leaf));

	ASSERT_EQ(t.leaves[2].leaf->header.count, 0);
	ASSERT_EQ(i0->header.childrenNeedingSplit, 1);
	ASSERT_EQ(root->header.childrenNeedingSplit, 1);
}

// Removing an empty leaf frees a slot of its parent: the parent stops
// needing a split and the root stops counting it.
TEST_CASE("/resolver/TempConflictBTree/RemoveEmptyChildPropagatesSplitCleared") {
	TestTree t;
	InternalBlock* i0 = fullLeafUnderCriticalNode(t);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 100);
	ASSERT(Access::refreshSeparator(t.tree, i0, 2));

	ASSERT_EQ(i0->header.count, InternalCapacity - 2);
	ASSERT_EQ(i0->header.childrenNeedingSplit, 1);
	ASSERT(!needsSplit(&i0->header));
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	checkTree(t);
}

namespace {

// root -> [I0, I1]; I0 -> [L0, L1, L2]; I1 -> [L3, L4, L5]. The separator of
// I1 takes the CV of the first entry of L3.
void threeLevels(TestTree& t, Version l3, Version l4, Version l5) {
	for (unsigned i = 0; i < 6; ++i) {
		const unsigned first = 10 + 20 * i;
		const Version cv = i == 3 ? l3 : i == 4 ? l4 : i == 5 ? l5 : 50;
		t.addLeaf(i == 0 ? Bytes{} : key(first),
				  withCVs(entriesFrom(first, 2, 0), {cv, cv}));
	}
	t.build(3);
}

} // namespace

// The first two grandchildren under the refreshed separator are expired:
// both are removed and the separator comes from the third one.
TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorSkipsExpiredGrandchildren") {
	TestTree t;
	threeLevels(t, 5, 8, 40);
	InternalBlock* root = rootNode(t);
	auto* i1 = reinterpret_cast<InternalBlock*>(childAt(root, 1));

	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparator(t.tree, root, 1));

	ASSERT_EQ(root->header.count, 2);
	ASSERT_EQ(i1->header.count, 1);
	ASSERT(childAt(i1, 0) == &t.leaves[5].leaf->header);
	const auto slot = root->header.order[1];
	ASSERT_EQ(root->separatorsCV[slot], 40);
	ASSERT_EQ(t.keys().compareKeys(asRef(key(110)), root->separators[slot]), 0);
	ASSERT(t.leaves[2].leaf->right == t.leaves[5].leaf);
	checkTree(t);
}

// Every grandchild is expired: the internal child is removed too.
TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorRemovesEmptyInternalChild") {
	TestTree t;
	threeLevels(t, 5, 8, 9);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparator(t.tree, root, 1));

	ASSERT_EQ(root->header.count, 1);
	ASSERT(t.leaves[2].leaf->right == nullptr);
	checkTree(t);
}

TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorsRefreshesWholeNode") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {5, 30, 30}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {2, 3, 4}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 3, 0), {30, 6, 30}));
	t.addLeaf(key(70), withCVs(entriesFrom(70, 3, 0), {7, 8, 35}));
	t.addLeaf(key(90), withCVs(entriesFrom(90, 3, 0), {1, 2, 3}));
	t.build(5);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparators(t.tree, root));

	// Leaves 1 and 4 were emptied and removed, and leaves 0 and 3 lost their
	// expired entries. Leaf 2 is not cleaned: its separator, with the CV 30
	// of its first entry, has not expired, so its entry with CV 6 stays.
	ASSERT_EQ(root->header.count, 3);
	for (std::size_t r = 0; r < root->header.count; ++r) {
		ASSERT(root->separatorsCV[root->header.order[r]] > 10);
	}
	ASSERT_EQ(leafEntries(t, t.leaves[2].leaf).size(), 3);
	const std::vector<Entry> entries = checkTree(t);
	ASSERT_EQ(entries.size(), 6);

	ASSERT(!Access::refreshSeparators(t.tree, root));
}

TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorsCanEmptyNode") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 2, 0), {1, 2}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 2, 0), {3, 4}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 2, 0), {5, 6}));
	t.build(3);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparators(t.tree, root));
	ASSERT_EQ(root->header.count, 0);
	ASSERT_EQ(root->header.freeSlots, InternalSlotMask);
}

namespace {

// Keys with a 16-bit value, so that trees can hold many full leaves. They
// sort with key(): {'k', hi, lo} falls between key(hi) and key(hi + 1).
Bytes wideKey(unsigned value) {
	return Bytes{'k', static_cast<uint8_t>(value >> 8),
				 static_cast<uint8_t>(value)};
}

// As entriesFrom, with wide keys.
std::vector<Entry> wideEntriesFrom(unsigned first, unsigned count, Version cv) {
	std::vector<Entry> entries;
	for (unsigned i = 0; i < count; ++i) {
		const unsigned v = first + 2 * i;
		entries.push_back({wideKey(v),
						   v % 4 == 0 ? keyAfter(wideKey(v)) : wideKey(v + 1),
						   cv + static_cast<Version>(i)});
	}
	return entries;
}

// Adds one leaf per element: full leaves where full[i], two entries otherwise.
void addWideLeaves(TestTree& t, const std::vector<bool>& full) {
	unsigned first = 10;
	for (std::size_t i = 0; i < full.size(); ++i) {
		const unsigned count = full[i] ? LeafCapacity : 2;
		t.addLeaf(i == 0 ? Bytes{} : wideKey(first),
				  wideEntriesFrom(first, count, 1));
		first += 2 * count;
	}
	ASSERT(first < 0x10000);
}

std::vector<Entry> modelEntries(const TestTree& t) {
	std::vector<Entry> entries;
	for (const LeafModel& l : t.leaves) {
		entries.insert(entries.end(), l.entries.begin(), l.entries.end());
	}
	return entries;
}

void checkSameEntries(const std::vector<Entry>& actual,
					  const std::vector<Entry>& expected) {
	ASSERT_EQ(actual.size(), expected.size());
	for (std::size_t i = 0; i < actual.size(); ++i) {
		ASSERT(actual[i].begin == expected[i].begin);
		ASSERT(actual[i].end == expected[i].end);
		ASSERT_EQ(actual[i].cv, expected[i].cv);
	}
}

// The rank at which an internal split cuts: the shortest prefix whose
// weight, two for a child needing a split and one otherwise, reaches half
// the total weight.
std::size_t expectedCut(const InternalBlock* node) {
	int remaining = (node->header.count + node->header.childrenNeedingSplit) / 2;
	std::size_t cut = 0;
	while (remaining > 0) {
		remaining -= needsSplit(node->children[node->header.order[cut]]) ? 2 : 1;
		++cut;
	}
	return cut;
}

struct Position {
	LeafBlock* leaf;
	std::size_t rank;
	bool found;
	BNodeHeader* nodeNeedsSplit;
};

Position find(TestTree& t, const Bytes& k) {
	Position p{nullptr, 999, false, nullptr};
	p.found = Access::findKey(t.tree, asRef(k), p.leaf, p.rank, p.nodeNeedsSplit);
	return p;
}

// Builds a tree with makeTree, finds k, splits the leaf found, and checks
// that the leaf and rank returned by split are those findKey returns on the
// split tree. Also checks the whole tree and that no entry was lost.
template<typename MakeTree>
void checkLeafSplitKeepsPosition(MakeTree makeTree, const Bytes& k) {
	TestTree t;
	makeTree(t);
	const std::vector<Entry> entries = modelEntries(t);

	Position before = find(t, k);
	ASSERT(before.leaf != nullptr);
	ASSERT(before.nodeNeedsSplit == &before.leaf->header);
	LeafBlock* leaf = before.leaf;
	std::size_t rank = before.rank;
	Access::split(t.tree, &leaf->header, leaf, rank, before.found, 50);

	const Position after = find(t, k);
	ASSERT_EQ(after.found, before.found);
	ASSERT(leaf == after.leaf);
	ASSERT_EQ(rank, after.rank);
	ASSERT(after.nodeNeedsSplit == nullptr);
	checkSameEntries(checkTree(t), entries);
}

// Keys to search for in a full leaf of wide keys starting at first: every
// begin and a key after each begin. A key before the first reaches this
// leaf only when it is the first leaf.
std::vector<Bytes> wideSearchKeys(unsigned first, unsigned count,
								  bool firstLeaf) {
	std::vector<Bytes> keys;
	if (firstLeaf) {
		keys.push_back(wideKey(first - 1));
	}
	for (unsigned i = 0; i < count; ++i) {
		keys.push_back(wideKey(first + 2 * i));
		keys.push_back(keyAfter(wideKey(first + 2 * i)));
	}
	return keys;
}

} // namespace

// The header overload agrees with the typed ones: a leaf needs a split only
// when full, an internal node when its free slots equal its children
// needing a split.
TEST_CASE("/resolver/TempConflictBTree/NeedsSplitOfHeaderMatchesNodeKind") {
	TestTree t;
	LeafBlock* leaf = t.nodes().allocateLeaf(0);
	InternalBlock* internal = t.nodes().allocateInternal(0);

	for (std::uint64_t freeSlots : {LeafSlotMask, std::uint64_t{1},
									std::uint64_t{1} << (LeafCapacity - 1),
									std::uint64_t{0}}) {
		leaf->header.freeSlots = freeSlots;
		const bool expected = freeSlots == 0;
		ASSERT_EQ(Access::needsSplit(leaf), expected);
		ASSERT_EQ(Access::needsSplit(&leaf->header), expected);
	}

	for (std::size_t used = 0; used <= InternalCapacity; ++used) {
		internal->header.freeSlots = InternalSlotMask >> used << used;
		const std::size_t free = InternalCapacity - used;
		for (std::size_t counted = 0; counted <= std::min(used, free); ++counted) {
			internal->header.childrenNeedingSplit =
				static_cast<std::uint8_t>(counted);
			const bool expected = counted == free;
			ASSERT_EQ(Access::needsSplit(internal), expected);
			ASSERT_EQ(Access::needsSplit(&internal->header), expected);
		}
	}
}

// findKey returns the deepest node on the path that needs a split: the leaf
// when it is full, otherwise the deepest internal node, or nullptr.
TEST_CASE("/resolver/TempConflictBTree/FindKeyReturnsDeepestNodeNeedingSplit") {
	// root -> [I0, I1]; I0 has 25 leaves, the last 15 full, so it needs a
	// split (25 + 15 == InternalCapacity); I1 has 5 leaves and does not.
	static_assert(InternalCapacity == 40);
	std::vector<bool> full(30, false);
	for (std::size_t i = 10; i < 25; ++i) {
		full[i] = true;
	}
	TestTree t;
	addWideLeaves(t, full);
	t.build(25);
	InternalBlock* root = rootNode(t);
	BNodeHeader* i0 = childAt(root, 0);
	ASSERT(Access::needsSplit(i0));
	ASSERT(!Access::needsSplit(childAt(root, 1)));
	ASSERT(!Access::needsSplit(&root->header));

	for (std::size_t i = 0; i < t.leaves.size(); ++i) {
		const Bytes k = i == 0 ? wideKey(10) : t.leaves[i].lowerBound;
		const Position p = find(t, k);
		ASSERT(p.leaf == t.leaves[i].leaf);
		BNodeHeader* expected = full[i] ? &t.leaves[i].leaf->header
							  : i < 25  ? i0
										: nullptr;
		ASSERT(p.nodeNeedsSplit == expected);
	}
	checkTree(t);
}

// A full root leaf splits into two leaves under a new root. For every key,
// split returns the position findKey returns on the split tree.
TEST_CASE("/resolver/TempConflictBTree/SplitRootLeaf") {
	const auto makeTree = [](TestTree& t) {
		t.addLeaf({}, wideEntriesFrom(10, LeafCapacity, 1));
		t.build(2);
	};

	TestTree t;
	makeTree(t);
	LeafBlock* old = t.leaves[0].leaf;
	LeafBlock* leaf = old;
	std::size_t rank = 0;
	Access::split(t.tree, &old->header, leaf, rank, false, 50);

	InternalBlock* root = rootNode(t);
	ASSERT_EQ(root->header.count, 2);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	ASSERT(root->header.parent == nullptr);
	ASSERT_EQ(root->header.maxCV, 50);
	ASSERT(childAt(root, 0) == &old->header);
	auto* sibling = reinterpret_cast<LeafBlock*>(childAt(root, 1));
	ASSERT_EQ(old->header.count, LeafCapacity / 2);
	ASSERT_EQ(sibling->header.count, LeafCapacity - LeafCapacity / 2);
	ASSERT(old->left == nullptr && old->right == sibling);
	ASSERT(sibling->left == old && sibling->right == nullptr);
	ASSERT_EQ(sibling->header.maxCV, 50);

	// The new separator is the first begin of the sibling, with its CV.
	const auto slot = root->header.order[1];
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(10 + 2 * (LeafCapacity / 2))),
								   root->separators[slot]), 0);
	ASSERT_EQ(root->separatorsCV[slot], 1 + LeafCapacity / 2);
	ASSERT_EQ(root->maxSubtreesCV[slot], 50);
	checkSameEntries(checkTree(t), modelEntries(t));

	for (const Bytes& k : wideSearchKeys(10, LeafCapacity, true)) {
		checkLeafSplitKeepsPosition(makeTree, k);
	}
}

// A full leaf under a parent: the sibling is linked between the leaf and its
// right neighbour, and the parent stops counting the leaf.
TEST_CASE("/resolver/TempConflictBTree/SplitLeafUnderParent") {
	const auto makeTree = [](TestTree& t) {
		addWideLeaves(t, {false, true, false});
		t.build(3);
	};

	TestTree t;
	makeTree(t);
	InternalBlock* root = rootNode(t);
	ASSERT_EQ(root->header.childrenNeedingSplit, 1);
	LeafBlock* old = t.leaves[1].leaf;
	LeafBlock* leaf = old;
	std::size_t rank = 3;
	Access::split(t.tree, &old->header, leaf, rank, false, 50);

	ASSERT(rootNode(t) == root);
	ASSERT_EQ(root->header.count, 4);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	ASSERT_EQ(root->header.maxCV, 50);
	ASSERT(childAt(root, 1) == &old->header);
	auto* sibling = reinterpret_cast<LeafBlock*>(childAt(root, 2));
	ASSERT(childAt(root, 3) == &t.leaves[2].leaf->header);
	ASSERT(old->right == sibling && sibling->left == old);
	ASSERT(sibling->right == t.leaves[2].leaf);
	ASSERT(t.leaves[2].leaf->left == sibling);
	ASSERT(leaf == old);
	ASSERT_EQ(rank, 3);
	checkSameEntries(checkTree(t), modelEntries(t));

	for (const Bytes& k : wideSearchKeys(14, LeafCapacity, false)) {
		checkLeafSplitKeepsPosition(makeTree, k);
	}
}

// Splitting a full leaf whose parent also needs a split uses the slot
// reserved for it: the parent becomes full and still needs a split.
TEST_CASE("/resolver/TempConflictBTree/SplitLeafUnderParentNeedingSplit") {
	std::vector<bool> full(InternalCapacity - 1, false);
	full[5] = true;
	TestTree t;
	addWideLeaves(t, full);
	t.build(InternalCapacity - 1);
	InternalBlock* root = rootNode(t);
	ASSERT(Access::needsSplit(root));
	ASSERT(find(t, t.leaves[5].lowerBound).nodeNeedsSplit ==
		   &t.leaves[5].leaf->header);

	LeafBlock* leaf = t.leaves[5].leaf;
	std::size_t rank = 0;
	Access::split(t.tree, &leaf->header, leaf, rank, false, 50);

	ASSERT_EQ(root->header.count, InternalCapacity);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	ASSERT(!root->header.hasSpace());
	ASSERT(Access::needsSplit(root));
	ASSERT(find(t, t.leaves[5].lowerBound).nodeNeedsSplit == &root->header);
	checkSameEntries(checkTree(t), modelEntries(t));
}

// An internal root needing a split is cut by weight: each child needing a
// split counts twice. Both halves stop needing a split, every moved child
// points to the new node, and leaf and rank are left unchanged.
TEST_CASE("/resolver/TempConflictBTree/SplitRootInternal") {
	static_assert(InternalCapacity == 40);
	std::vector<std::vector<bool>> layouts;
	layouts.push_back(std::vector<bool>(40, false));     // full, no child counted
	std::vector<bool> layout(30, false);
	std::fill(layout.begin(), layout.begin() + 10, true);
	layouts.push_back(layout);                            // counted children first
	std::reverse(layout.begin(), layout.end());
	layouts.push_back(layout);                            // counted children last
	layout.assign(30, false);
	for (std::size_t i = 0; i < 30; i += 3) {
		layout[i] = true;
	}
	layouts.push_back(layout);                            // spread
	layouts.push_back(std::vector<bool>(20, true));       // all counted

	for (const std::vector<bool>& full : layouts) {
		TestTree t;
		addWideLeaves(t, full);
		t.build(full.size());
		InternalBlock* old = rootNode(t);
		ASSERT(Access::needsSplit(old));
		const std::size_t cut = expectedCut(old);
		const std::size_t count = old->header.count;
		std::vector<BNodeHeader*> children;
		for (std::size_t r = 0; r < count; ++r) {
			children.push_back(childAt(old, r));
		}

		LeafBlock* leaf = t.leaves[0].leaf;
		std::size_t rank = 1;
		Access::split(t.tree, &old->header, leaf, rank, false, 50);
		ASSERT(leaf == t.leaves[0].leaf);
		ASSERT_EQ(rank, 1);

		InternalBlock* root = rootNode(t);
		ASSERT(root != old);
		ASSERT_EQ(root->header.count, 2);
		ASSERT_EQ(root->header.childrenNeedingSplit, 0);
		ASSERT(childAt(root, 0) == &old->header);
		auto* sibling = reinterpret_cast<InternalBlock*>(childAt(root, 1));
		ASSERT_EQ(old->header.count, cut);
		ASSERT_EQ(sibling->header.count, count - cut);
		ASSERT(!Access::needsSplit(old));
		ASSERT(!Access::needsSplit(sibling));
		ASSERT_EQ(sibling->header.maxCV, 50);
		for (std::size_t r = 0; r < count; ++r) {
			ASSERT(r < cut ? childAt(old, r) == children[r]
						   : childAt(sibling, r - cut) == children[r]);
		}
		// The sibling's slots are filled in rank order.
		for (std::size_t r = 0; r < sibling->header.count; ++r) {
			ASSERT_EQ(sibling->header.order[r], r);
		}
		ASSERT_EQ(sibling->header.freeSlots,
				  InternalSlotMask >> sibling->header.count << sibling->header.count);
		checkSameEntries(checkTree(t), modelEntries(t));
	}
}

// An internal node under a parent: the sibling goes right after it in the
// parent, the parent stops counting it, and the sibling's CV reaches the
// root.
TEST_CASE("/resolver/TempConflictBTree/SplitInternalUnderParent") {
	static_assert(InternalCapacity == 40);
	std::vector<bool> full(30, false);
	for (std::size_t i = 10; i < 25; ++i) {
		full[i] = true;
	}
	TestTree t;
	addWideLeaves(t, full);
	t.build(25);
	InternalBlock* root = rootNode(t);
	auto* old = reinterpret_cast<InternalBlock*>(childAt(root, 0));
	BNodeHeader* right = childAt(root, 1);
	ASSERT_EQ(root->header.childrenNeedingSplit, 1);
	const std::size_t cut = expectedCut(old);

	LeafBlock* leaf = nullptr;
	std::size_t rank = 0;
	Access::split(t.tree, &old->header, leaf, rank, false, 50);

	ASSERT(rootNode(t) == root);
	ASSERT_EQ(root->header.count, 3);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	ASSERT(childAt(root, 0) == &old->header);
	ASSERT(childAt(root, 2) == right);
	auto* sibling = reinterpret_cast<InternalBlock*>(childAt(root, 1));
	ASSERT_EQ(old->header.count, cut);
	ASSERT_EQ(sibling->header.count, 25 - cut);
	ASSERT_EQ(old->header.childrenNeedingSplit + sibling->header.childrenNeedingSplit, 15);
	ASSERT_EQ(root->maxSubtreesCV[sibling->header.parentSlot], 50);
	ASSERT_EQ(root->header.maxCV, 50);
	ASSERT(t.nodes().oldestNode() != &root->header);
	checkSameEntries(checkTree(t), modelEntries(t));

	// Every leaf is still reached, and none of the path needs a split
	// except the full leaves themselves.
	for (std::size_t i = 0; i < t.leaves.size(); ++i) {
		const Position p = find(t, i == 0 ? wideKey(10) : t.leaves[i].lowerBound);
		ASSERT(p.leaf == t.leaves[i].leaf);
		ASSERT(p.nodeNeedsSplit ==
			   (full[i] ? &t.leaves[i].leaf->header : nullptr));
	}
}
