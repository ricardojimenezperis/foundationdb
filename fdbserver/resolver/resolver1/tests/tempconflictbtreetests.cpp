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
	static void updateSeparator(TempConflictBTree& t, LeafBlock* leaf) {
		t.propagateUpdates(leaf, 0, true, false);
	}
	static void updateSeparator(TempConflictBTree& t, InternalBlock* node) {
		t.propagateUpdates(node, 0, true, false);
	}
	static void addWrite(TempConflictBTree& t, const StringRef& begin,
						 const StringRef& end, Version cv) {
		t.addWrite({begin, end}, cv);
	}
	static BNodeHeader* detach(TempConflictBTree& t, std::size_t rank,
							   InternalBlock* node) {
		return t.eraseAt(rank, node);
	}
	static void eraseAt(TempConflictBTree& t, std::size_t& rank, LeafBlock*& leaf) {
		t.eraseAt(rank, leaf);
	}
	static void preserveRightExterior(TempConflictBTree& t, const StringRef& end,
									  LeafBlock* leaf, std::size_t rank) {
		t.preserveRightExterior(end, leaf, rank);
	}
	static void insertAt(TempConflictBTree& t, std::size_t& rank,
						 const RangeRef& range, Version cv, LeafBlock*& leaf) {
		t.insertAt(rank, range, cv, leaf);
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

// A leaf needs a split when it has at most one free slot; an internal node
// when every free slot may be taken by the split of a child that needs one.
bool needsSplit(const BNodeHeader* node) {
	if (node->kind == BNodeKind::Leaf) {
		return node->freeSlotCount() <= 1;
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

// A tree built directly from nodes. Entries are inserted in reverse order,
// always at rank 0, so that order is never the identity.
class TestTree {
public:
	explicit TestTree(std::size_t budget = 16 * PageSize) : tree(budget) {}

	TempConflictBTree tree;
	std::vector<LeafModel> leaves;
	// Newest maximum CV of the nodes created, since nodes must be created
	// in nondecreasing CV order and inserting an entry raises a leaf's CV.
	Version newestCV = 0;

	KeyStore& keys() { return Access::keys(tree); }
	BNodeStore& nodes() { return Access::nodes(tree); }

	LeafBlock* addLeaf(const Bytes& lowerBound, std::vector<Entry> entries) {
		LeafBlock* leaf = nodes().allocateLeaf(newestCV);
		ASSERT(leaf != nullptr);

		for (auto e = entries.rbegin(); e != entries.rend(); ++e) {
			const std::optional<RangeRef> range =
				keys().appendRange(asRef(e->begin), asRef(e->end), e->cv);
			ASSERT(range.has_value());
			nodes().insertAt(0, *range, e->cv, leaf);
		}
		newestCV = std::max(newestCV, leaf->header.maxCV);

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
			// CV of the first entry below, as refreshing sets it.
			Version cv;
		};
		std::vector<Child> level;
		for (const LeafModel& l : leaves) {
			// Only a root leaf may be empty.
			ASSERT(!l.entries.empty() || leaves.size() == 1);
			level.push_back({&l.leaf->header, l.lowerBound,
							 l.entries.empty() ? 0 : l.entries.front().cv});
		}

		while (level.size() > 1) {
			std::vector<Child> parents;
			for (std::size_t i = 0; i < level.size(); i += fanout) {
				InternalBlock* internal = nodes().allocateInternal(newestCV);
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

namespace {

// Leaves with 3, 1, 2, 1, 1 and 4 entries, under two internal nodes, so
// that moving between entries crosses leaves and internal nodes.
TestTree& variedLeaves(TestTree& t) {
	t.addLeaf({}, entriesFrom(10, 3, 1));
	t.addLeaf(key(20), entriesFrom(20, 1, 1));
	t.addLeaf(key(30), entriesFrom(30, 2, 1));
	t.addLeaf(key(40), entriesFrom(40, 1, 1));
	t.addLeaf(key(50), entriesFrom(50, 1, 1));
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
	variedLeaves(t);
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
	variedLeaves(t);
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

// From the first entry of a leaf both directions cross to the neighbours,
// also across internal nodes; from the end position of a leaf, nextRank
// goes right.
TEST_CASE("/resolver/TempConflictBTree/RankMovesBetweenLeaves") {
	TestTree t;
	variedLeaves(t);
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
	ASSERT(leaf == t.leaves[4].leaf);
	ASSERT_EQ(rank, 0);
	ASSERT_EQ(slot, leaf->header.order[0]);

	leaf = t.leaves[0].leaf;
	rank = leaf->header.count;
	ASSERT(Access::nextRank(t.tree, leaf, rank, slot));
	ASSERT(leaf == t.leaves[1].leaf);
	ASSERT_EQ(rank, 0);

	// An empty root leaf has no entries in either direction.
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

		// Ten leaves over k(10)..k(209), with gaps between them and random
		// CVs. Some leaves hold a single entry.
		for (unsigned i = 0; i < 10; ++i) {
			const unsigned first = 10 + 20 * i;
			std::vector<Entry> entries =
				entriesFrom(first, i % 4 == 2 ? 1 : 1 + next() % 6, 0);
			for (Entry& e : entries) {
				e.cv = static_cast<Version>(next() % 50);
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
		// Only the root leaf may be empty.
		ASSERT(node->parent == nullptr || leaf->header.count > 0);
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
		ASSERT(internal->children[slot]->parent == internal);
		ASSERT_EQ(internal->children[slot]->parentSlot, slot);
		ASSERT(internal->maxSubtreesCV[slot] >= internal->children[slot]->maxCV);
		ASSERT(node->maxCV >= internal->children[slot]->maxCV);
		const std::size_t before = entries.size();
		checkSubtree(t, internal->children[slot], leaves, entries);

		// Every begin in this child is at least its separator, and every
		// begin before it is below it. Entries are checked to be in order,
		// so the entries on either side of the separator are enough.
		const KeyRef& separator = internal->separators[slot];
		if (r > 0 && before > 0) {
			ASSERT(t.keys().compareKeys(asRef(entries[before - 1].begin), separator) < 0);
		}
		if (r > 0 && before < entries.size()) {
			ASSERT(t.keys().compareKeys(asRef(entries[before].begin), separator) >= 0);
		}
	}
}

// Checks the structure of the tree and its leaf links, and returns its
// entries in key order.
std::vector<Entry> checkStructure(TestTree& t) {
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
	return entries;
}

// Checks the whole tree: structure, leaf links and reads against a
// brute-force scan of the entries left in it.
std::vector<Entry> checkTree(TestTree& t) {
	const std::vector<Entry> entries = checkStructure(t);
	for (unsigned b = 0; b <= 230; b += 3) {
		const Bytes begin = key(b);
		for (unsigned e = b + 1; e <= 231; e += 7) {
			const Bytes end = key(e);
			// Reads are admitted only at or above reclaimCV, and may clean
			// the tree, which never changes their outcome.
			for (Version version : {t.tree.reclaimCV(), Version{0},
									Version{15}, Version{40}}) {
				if (version < t.tree.reclaimCV()) {
					continue;
				}
				// The entries are in key order: stop past the read.
				bool expected = false;
				for (const Entry& entry : entries) {
					if (expected || !(entry.begin < end)) {
						break;
					}
					expected = begin < entry.end && entry.cv > version;
				}
				ASSERT_EQ(t.hasConflict(begin, end, version), expected);
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

// root -> [I0, I1]. I0 has a single free slot and its second child is a leaf
// with entries CVs 1..entries, full by default, so I0 needs a split and the
// root counts it when that leaf needs one. The other leaves hold one entry
// each, with CV 100.
InternalBlock* fullLeafUnderCriticalNode(TestTree& t,
										 unsigned entries = LeafCapacity) {
	constexpr std::size_t children = InternalCapacity - 1;
	t.addLeaf({}, entriesFrom(10, 1, 100));
	t.addLeaf(key(12), entriesFrom(12, entries, 1));
	unsigned first = 12 + 2 * entries;
	for (std::size_t i = 2; i <= children; ++i, first += 2) {
		t.addLeaf(key(first), entriesFrom(first, 1, 100));
	}
	ASSERT(first <= 231);
	t.build(children);

	InternalBlock* root = rootNode(t);
	ASSERT_EQ(root->header.count, 2);
	auto* i0 = reinterpret_cast<InternalBlock*>(childAt(root, 0));
	ASSERT_EQ(i0->header.count, children);
	const bool leafNeedsSplit = entries + 1 >= LeafCapacity;
	ASSERT_EQ(i0->header.childrenNeedingSplit, leafNeedsSplit);
	ASSERT_EQ(needsSplit(&i0->header), leafNeedsSplit);
	ASSERT_EQ(root->header.childrenNeedingSplit, leafNeedsSplit);
	ASSERT(!needsSplit(&root->header));
	return i0;
}

} // namespace

// When cleaning leaves two free slots in the full leaf, it stops needing a
// split, and so does its parent, which no longer counts it: both counts go
// down.
TEST_CASE("/resolver/TempConflictBTree/CleanExpiredPropagatesSplitCleared") {
	TestTree t;
	InternalBlock* i0 = fullLeafUnderCriticalNode(t);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 2);
	ASSERT(Access::cleanExpired(t.tree, t.leaves[1].leaf));

	ASSERT_EQ(t.leaves[1].leaf->header.freeSlotCount(), 2);
	ASSERT_EQ(i0->header.childrenNeedingSplit, 0);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	checkTree(t);
}

// Cleaning a leaf that was not full leaves the counts unchanged. The leaf
// is left empty, which its caller would then have to resolve.
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

// CV of the splits in these tests, newer than every entry.
constexpr Version SplitCV = 500;

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
// split tree. Also checks the structure of the tree and that no entry was
// lost.
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
	Access::split(t.tree, &leaf->header, leaf, rank, before.found, SplitCV);

	const Position after = find(t, k);
	ASSERT_EQ(after.found, before.found);
	ASSERT(leaf == after.leaf);
	ASSERT_EQ(rank, after.rank);
	ASSERT(after.nodeNeedsSplit == nullptr);
	checkSameEntries(checkStructure(t), entries);
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

// The header overload agrees with the typed ones: a leaf needs a split when
// it has at most one free slot, an internal node when its free slots equal
// its children needing a split.
TEST_CASE("/resolver/TempConflictBTree/NeedsSplitOfHeaderMatchesNodeKind") {
	TestTree t;
	LeafBlock* leaf = t.nodes().allocateLeaf(0);
	InternalBlock* internal = t.nodes().allocateInternal(0);

	std::vector<std::uint64_t> leafPatterns{
		std::uint64_t{1}, std::uint64_t{1} << (LeafCapacity - 1),
		std::uint64_t{1} | std::uint64_t{1} << (LeafCapacity - 1)};
	for (std::size_t used = 0; used <= LeafCapacity; ++used) {
		leafPatterns.push_back(LeafSlotMask >> used << used);
	}
	for (std::uint64_t freeSlots : leafPatterns) {
		leaf->header.freeSlots = freeSlots;
		const bool expected = std::popcount(freeSlots) <= 1;
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
	Access::split(t.tree, &old->header, leaf, rank, false, SplitCV);

	InternalBlock* root = rootNode(t);
	ASSERT_EQ(root->header.count, 2);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	ASSERT(root->header.parent == nullptr);
	ASSERT_EQ(root->header.maxCV, SplitCV);
	ASSERT(childAt(root, 0) == &old->header);
	auto* sibling = reinterpret_cast<LeafBlock*>(childAt(root, 1));
	ASSERT_EQ(old->header.count, LeafCapacity / 2);
	ASSERT_EQ(sibling->header.count, LeafCapacity - LeafCapacity / 2);
	ASSERT(old->left == nullptr && old->right == sibling);
	ASSERT(sibling->left == old && sibling->right == nullptr);
	ASSERT_EQ(sibling->header.maxCV, SplitCV);

	// The new separator is the first begin of the sibling, with its CV.
	const auto slot = root->header.order[1];
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(10 + 2 * (LeafCapacity / 2))),
								   root->separators[slot]), 0);
	ASSERT_EQ(root->separatorsCV[slot], 1 + LeafCapacity / 2);
	ASSERT_EQ(root->maxSubtreesCV[slot], SplitCV);
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
	Access::split(t.tree, &old->header, leaf, rank, false, SplitCV);

	ASSERT(rootNode(t) == root);
	ASSERT_EQ(root->header.count, 4);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	ASSERT_EQ(root->header.maxCV, SplitCV);
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
	Access::split(t.tree, &leaf->header, leaf, rank, false, SplitCV);

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
		Access::split(t.tree, &old->header, leaf, rank, false, SplitCV);
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
		ASSERT_EQ(sibling->header.maxCV, SplitCV);
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
	Access::split(t.tree, &old->header, leaf, rank, false, SplitCV);

	ASSERT(rootNode(t) == root);
	ASSERT_EQ(root->header.count, 3);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	ASSERT(childAt(root, 0) == &old->header);
	ASSERT(childAt(root, 2) == right);
	auto* sibling = reinterpret_cast<InternalBlock*>(childAt(root, 1));
	ASSERT_EQ(old->header.count, cut);
	ASSERT_EQ(sibling->header.count, 25 - cut);
	ASSERT_EQ(old->header.childrenNeedingSplit + sibling->header.childrenNeedingSplit, 15);
	ASSERT_EQ(root->maxSubtreesCV[sibling->header.parentSlot], SplitCV);
	ASSERT_EQ(root->header.maxCV, SplitCV);
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

// Leaves with LeafCapacity - 2, LeafCapacity - 1 and LeafCapacity entries:
// only the last two need a split, are counted by their parent and are
// returned by findKey.
TEST_CASE("/resolver/TempConflictBTree/LeafNeedsSplitWithOneFreeSlot") {
	const unsigned counts[] = {LeafCapacity - 2, LeafCapacity - 1, LeafCapacity};
	TestTree t;
	unsigned first = 10;
	for (std::size_t i = 0; i < 3; ++i) {
		t.addLeaf(i == 0 ? Bytes{} : wideKey(first),
				  wideEntriesFrom(first, counts[i], 1));
		first += 2 * counts[i];
	}
	t.build(3);
	InternalBlock* root = rootNode(t);
	ASSERT_EQ(root->header.childrenNeedingSplit, 2);

	for (std::size_t i = 0; i < 3; ++i) {
		LeafBlock* leaf = t.leaves[i].leaf;
		ASSERT_EQ(leaf->header.freeSlotCount(), LeafCapacity - counts[i]);
		ASSERT_EQ(Access::needsSplit(leaf), i > 0);
		ASSERT_EQ(Access::needsSplit(&leaf->header), i > 0);
		const Position p =
			find(t, i == 0 ? wideKey(10) : t.leaves[i].lowerBound);
		ASSERT(p.leaf == leaf);
		ASSERT(p.nodeNeedsSplit == (i > 0 ? &leaf->header : nullptr));
	}
	checkTree(t);
}

// Cleaning a leaf reports its transition once, and only when it goes from
// at most one free slot to at least two. The parent has a single free slot,
// so the transition also reaches the root.
TEST_CASE("/resolver/TempConflictBTree/CleanExpiredReportsLeafTransitionOnce") {
	struct Case {
		unsigned entries;
		Version reclaim;     // removes the entries with CVs 1..reclaim
		bool before;
		bool after;
	};
	const Case cases[] = {
		{LeafCapacity, 0, true, true},
		{LeafCapacity, 1, true, true},             // full -> one free
		{LeafCapacity, 2, true, false},            // full -> two free
		{LeafCapacity, 5, true, false},
		{LeafCapacity - 1, 1, true, false},        // one free -> two free
		{LeafCapacity - 1, LeafCapacity - 2, true, false},
		{LeafCapacity - 2, 1, false, false},
		{LeafCapacity - 2, 0, false, false},
	};
	for (const Case& c : cases) {
		TestTree t;
		InternalBlock* i0 = fullLeafUnderCriticalNode(t, c.entries);
		InternalBlock* root = rootNode(t);
		LeafBlock* leaf = t.leaves[1].leaf;
		ASSERT_EQ(Access::needsSplit(leaf), c.before);

		Access::setReclaimCV(t.tree, c.reclaim);
		ASSERT_EQ(Access::cleanExpired(t.tree, leaf), c.reclaim > 0);

		ASSERT_EQ(leaf->header.count, c.entries - c.reclaim);
		ASSERT_EQ(Access::needsSplit(leaf), c.after);
		ASSERT_EQ(i0->header.childrenNeedingSplit, c.after);
		ASSERT_EQ(Access::needsSplit(i0), c.after);
		ASSERT_EQ(root->header.childrenNeedingSplit, c.after);
		checkTree(t);
	}
}

// A leaf with one free slot splits into two halves of LeafCapacity / 2, and
// split keeps every position findKey returns.
TEST_CASE("/resolver/TempConflictBTree/SplitLeafWithOneFreeSlot") {
	constexpr unsigned entries = LeafCapacity - 1;
	const auto makeTree = [](TestTree& t) {
		t.addLeaf({}, wideEntriesFrom(10, 2, 1));
		t.addLeaf(wideKey(14), wideEntriesFrom(14, entries, 1));
		t.addLeaf(wideKey(14 + 2 * entries),
				  wideEntriesFrom(14 + 2 * entries, 2, 1));
		t.build(3);
	};

	TestTree t;
	makeTree(t);
	InternalBlock* root = rootNode(t);
	ASSERT_EQ(root->header.childrenNeedingSplit, 1);
	LeafBlock* old = t.leaves[1].leaf;
	LeafBlock* leaf = old;
	std::size_t rank = 0;
	Access::split(t.tree, &old->header, leaf, rank, false, SplitCV);

	auto* sibling = reinterpret_cast<LeafBlock*>(childAt(root, 2));
	ASSERT_EQ(old->header.count, entries / 2);
	ASSERT_EQ(sibling->header.count, entries - entries / 2);
	ASSERT(!Access::needsSplit(old));
	ASSERT(!Access::needsSplit(sibling));
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	checkSameEntries(checkTree(t), modelEntries(t));

	for (const Bytes& k : wideSearchKeys(14, entries, false)) {
		checkLeafSplitKeepsPosition(makeTree, k);
	}
}

namespace {

// Inserts [begin, end) with cv into leaf i of the tree at rank, and keeps the
// model up to date. Checks that leaf and rank still identify the entry.
void insertEntry(TestTree& t, std::size_t i, std::size_t rank,
				 const Bytes& begin, const Bytes& end, Version cv) {
	const std::optional<RangeRef> range =
		t.keys().appendRange(asRef(begin), asRef(end), cv);
	ASSERT(range.has_value());
	LeafBlock* leaf = t.leaves[i].leaf;
	std::size_t r = rank;
	Access::insertAt(t.tree, r, *range, cv, leaf);
	ASSERT(leaf == t.leaves[i].leaf);
	ASSERT_EQ(r, rank);
	const auto slot = leaf->header.order[rank];
	ASSERT_EQ(leaf->cv[slot], cv);
	ASSERT_EQ(t.keys().compareBegin(asRef(begin), leaf->ranges[slot]), 0);

	std::vector<Entry>& entries = t.leaves[i].entries;
	entries.insert(entries.begin() + rank, {begin, end, cv});
	t.newestCV = std::max(t.newestCV, cv);
}

// The separator of node in its parent.
const KeyRef& separatorOf(const BNodeHeader* node) {
	return node->parent->separators[node->parentSlot];
}

Version separatorCVOf(const BNodeHeader* node) {
	return node->parent->separatorsCV[node->parentSlot];
}

// Adds leaves with the given entry counts and wide keys. Every leaf but the
// first has a lower bound one below its first key, so that a key can be
// inserted before its first entry without leaving it.
void addGapLeaves(TestTree& t, const std::vector<unsigned>& counts) {
	unsigned first = 10;
	for (std::size_t i = 0; i < counts.size(); ++i) {
		t.addLeaf(i == 0 ? Bytes{} : wideKey(first - 1),
				  wideEntriesFrom(first, counts[i], 1));
		first += 2 * counts[i] + 2;
	}
}

// Order in which the fill tests insert LeafCapacity keys: 23 is coprime with
// LeafCapacity, so every key is inserted once, at the front, the back and
// in between.
std::vector<unsigned> fillOrder() {
	std::vector<unsigned> order;
	for (unsigned n = 0; n < LeafCapacity; ++n) {
		order.push_back(n * 23 % LeafCapacity);
	}
	return order;
}

std::size_t rankAmong(const std::vector<unsigned>& inserted, unsigned key) {
	return static_cast<std::size_t>(
		std::count_if(inserted.begin(), inserted.end(),
					  [key](unsigned k) { return k < key; }));
}

} // namespace

// An empty root leaf is filled in a scattered order. Each insertion raises
// the leaf's maximum CV, and the leaf needs a split from its last but one
// entry on.
TEST_CASE("/resolver/TempConflictBTree/InsertAtFillsRootLeaf") {
	TestTree t;
	t.addLeaf({}, {});
	t.build(2);
	LeafBlock* leaf = t.leaves[0].leaf;
	ASSERT(Access::root(t.tree) == &leaf->header);

	std::vector<unsigned> inserted;
	for (unsigned k : fillOrder()) {
		const Version cv = 100 + static_cast<Version>(inserted.size());
		insertEntry(t, 0, rankAmong(inserted, k), wideKey(10 + 2 * k),
					keyAfter(wideKey(10 + 2 * k)), cv);
		inserted.push_back(k);

		ASSERT_EQ(leaf->header.count, inserted.size());
		ASSERT_EQ(leaf->header.maxCV, cv);
		ASSERT(t.nodes().oldestNode() == &leaf->header);
		ASSERT_EQ(Access::needsSplit(leaf),
				  inserted.size() + 1 >= LeafCapacity);
		checkSameEntries(leafEntries(t, leaf), t.leaves[0].entries);
	}
	ASSERT(!leaf->header.hasSpace());
	checkTree(t);
}

// A leaf between two others, starting with a single entry, is filled in a
// scattered order. Its separator follows its first key, the parent's CVs
// follow the leaf's, and the parent counts the leaf once from its last but
// one entry on.
TEST_CASE("/resolver/TempConflictBTree/InsertAtFillsLeafUnderParent") {
	const std::vector<unsigned> order = fillOrder();
	const unsigned k0 = order.front();
	TestTree t;
	t.addLeaf({}, wideEntriesFrom(2, 2, 1));
	t.addLeaf(wideKey(10 + 2 * k0),
			  {{wideKey(10 + 2 * k0), keyAfter(wideKey(10 + 2 * k0)), 1}});
	t.addLeaf(wideKey(200), wideEntriesFrom(200, 2, 1));
	t.build(3);
	InternalBlock* root = rootNode(t);
	LeafBlock* leaf = t.leaves[1].leaf;

	std::vector<unsigned> inserted{k0};
	for (unsigned k : std::vector<unsigned>(order.begin() + 1, order.end())) {
		const Version cv = 100 + static_cast<Version>(inserted.size());
		const std::size_t rank = rankAmong(inserted, k);
		insertEntry(t, 1, rank, wideKey(10 + 2 * k),
					keyAfter(wideKey(10 + 2 * k)), cv);
		inserted.push_back(k);

		const auto first = *std::min_element(inserted.begin(), inserted.end());
		ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(10 + 2 * first)),
									   separatorOf(&leaf->header)), 0);
		if (rank == 0) {
			ASSERT_EQ(separatorCVOf(&leaf->header), cv);
		}
		ASSERT_EQ(leaf->header.maxCV, cv);
		ASSERT_EQ(root->maxSubtreesCV[leaf->header.parentSlot], cv);
		ASSERT_EQ(root->header.maxCV, cv);
		const bool needs = inserted.size() + 1 >= LeafCapacity;
		ASSERT_EQ(Access::needsSplit(leaf), needs);
		ASSERT_EQ(root->header.childrenNeedingSplit, needs);
		checkStructure(t);
	}
	checkSameEntries(checkTree(t), modelEntries(t));
}

// Every combination of the three updates an insertion may cause in the
// parent: the leaf starts needing a split, the separator changes because the
// entry goes first, and the maximum CV rises because the entry is newer.
TEST_CASE("/resolver/TempConflictBTree/InsertAtUpdateCombinations") {
	for (unsigned mask = 0; mask < 8; ++mask) {
		const bool split = mask & 1;
		const bool first = mask & 2;
		const bool newer = mask & 4;

		TestTree t;
		const unsigned count = split ? LeafCapacity - 2 : 3;
		addGapLeaves(t, {2, count, 2});
		t.build(3);
		InternalBlock* root = rootNode(t);
		LeafBlock* leaf = t.leaves[1].leaf;
		const auto slot = leaf->header.parentSlot;
		const KeyRef separator = separatorOf(&leaf->header);
		const Version separatorCV = separatorCVOf(&leaf->header);
		const Version leafCV = leaf->header.maxCV;
		const Version subtreeCV = root->maxSubtreesCV[slot];
		const Version rootCV = root->header.maxCV;
		ASSERT(!Access::needsSplit(leaf));
		ASSERT_EQ(root->header.childrenNeedingSplit, 0);

		// The leaf's entries start at 16, its lower bound is 15, and its
		// last entry is [16 + 2 (count - 1), +1).
		const unsigned last = 16 + 2 * (count - 1);
		const Bytes begin = first ? wideKey(15) : wideKey(last + 1);
		const Bytes end = keyAfter(begin);
		const Version cv = newer ? 500 : 1;
		ASSERT(newer == (cv > leafCV));
		insertEntry(t, 1, first ? 0 : count, begin, end, cv);

		ASSERT_EQ(Access::needsSplit(leaf), split);
		ASSERT_EQ(root->header.childrenNeedingSplit, split);
		if (first) {
			ASSERT_EQ(t.keys().compareKeys(asRef(begin), separatorOf(&leaf->header)), 0);
			ASSERT_EQ(separatorCVOf(&leaf->header), cv);
		} else {
			ASSERT(separatorOf(&leaf->header).firstBlock == separator.firstBlock);
			ASSERT_EQ(separatorOf(&leaf->header).keyOffset, separator.keyOffset);
			ASSERT_EQ(separatorCVOf(&leaf->header), separatorCV);
		}
		ASSERT_EQ(leaf->header.maxCV, newer ? cv : leafCV);
		ASSERT_EQ(root->maxSubtreesCV[slot], newer ? cv : subtreeCV);
		ASSERT_EQ(root->header.maxCV, newer ? cv : rootCV);
		checkSameEntries(checkTree(t), modelEntries(t));
	}
}

// An entry inserted first in the first leaf of a subtree changes the
// separators up to the first ancestor where its subtree is not the first.
TEST_CASE("/resolver/TempConflictBTree/InsertAtPropagatesSeparator") {
	// Leaves 0..2 under I0, 3..5 under I1. Each case inserts first in a leaf
	// and lists the levels whose separator changes: 1 for the leaf's own,
	// 2 also for its parent's.
	struct Case {
		std::size_t leaf;
		int levels;
	};
	for (const Case c : {Case{0, 2}, Case{1, 1}, Case{3, 2}, Case{4, 1}}) {
		TestTree t;
		addGapLeaves(t, {3, 3, 3, 3, 3, 3});
		t.build(3);
		InternalBlock* root = rootNode(t);
		LeafBlock* leaf = t.leaves[c.leaf].leaf;
		InternalBlock* parent = leaf->header.parent;
		const KeyRef parentSeparator = separatorOf(&parent->header);
		const Version parentSeparatorCV = separatorCVOf(&parent->header);

		// A key below the leaf's first entry, still within the leaf.
		const Bytes begin = c.leaf == 0
			? wideKey(9)
			: t.leaves[c.leaf].lowerBound;
		insertEntry(t, c.leaf, 0, begin, keyAfter(begin), 500);

		ASSERT_EQ(t.keys().compareKeys(asRef(begin), separatorOf(&leaf->header)), 0);
		ASSERT_EQ(separatorCVOf(&leaf->header), 500);
		if (c.levels == 2) {
			ASSERT_EQ(t.keys().compareKeys(asRef(begin), separatorOf(&parent->header)), 0);
			ASSERT_EQ(separatorCVOf(&parent->header), 500);
		} else {
			ASSERT(separatorOf(&parent->header).firstBlock == parentSeparator.firstBlock);
			ASSERT_EQ(separatorOf(&parent->header).keyOffset, parentSeparator.keyOffset);
			ASSERT_EQ(separatorCVOf(&parent->header), parentSeparatorCV);
		}
		ASSERT(root->header.parent == nullptr);
		checkSameEntries(checkTree(t), modelEntries(t));
	}
}

// A leaf that starts needing a split under a parent with a single free slot
// makes the parent need one too, and the root counts the parent.
TEST_CASE("/resolver/TempConflictBTree/InsertAtPropagatesNeedsSplit") {
	TestTree t;
	InternalBlock* i0 = fullLeafUnderCriticalNode(t, LeafCapacity - 2);
	InternalBlock* root = rootNode(t);
	ASSERT(!Access::needsSplit(i0));

	// After the last entry of the leaf and before the next leaf.
	const Entry& last = t.leaves[1].entries.back();
	insertEntry(t, 1, LeafCapacity - 2, last.end, keyAfter(last.end), 1);

	ASSERT(Access::needsSplit(t.leaves[1].leaf));
	ASSERT_EQ(i0->header.childrenNeedingSplit, 1);
	ASSERT(Access::needsSplit(i0));
	ASSERT_EQ(root->header.childrenNeedingSplit, 1);
	ASSERT(!Access::needsSplit(root));
	checkSameEntries(checkTree(t), modelEntries(t));

	// A further entry fills the leaf, which is already counted.
	const Entry& next = t.leaves[1].entries.back();
	insertEntry(t, 1, LeafCapacity - 1, next.end, keyAfter(next.end), 1);
	ASSERT(!t.leaves[1].leaf->header.hasSpace());
	ASSERT_EQ(i0->header.childrenNeedingSplit, 1);
	ASSERT_EQ(root->header.childrenNeedingSplit, 1);
	checkSameEntries(checkTree(t), modelEntries(t));
}

// A newer entry raises the maximum CV up to the root. A second one with the
// same CV, in another leaf under the same parent, stops at the parent,
// whose maximum CV is already that high.
TEST_CASE("/resolver/TempConflictBTree/InsertAtPropagatesMaxCV") {
	TestTree t;
	addGapLeaves(t, {3, 3, 3, 3, 3, 3});
	t.build(3);
	InternalBlock* root = rootNode(t);
	LeafBlock* a = t.leaves[3].leaf;
	LeafBlock* b = t.leaves[4].leaf;
	InternalBlock* parent = a->header.parent;
	ASSERT(b->header.parent == parent);

	const Entry& lastA = t.leaves[3].entries.back();
	insertEntry(t, 3, 3, lastA.end, keyAfter(lastA.end), 500);
	ASSERT_EQ(a->header.maxCV, 500);
	ASSERT_EQ(parent->maxSubtreesCV[a->header.parentSlot], 500);
	ASSERT_EQ(parent->header.maxCV, 500);
	ASSERT_EQ(root->maxSubtreesCV[parent->header.parentSlot], 500);
	ASSERT_EQ(root->header.maxCV, 500);
	checkTree(t);

	const Version before = parent->maxSubtreesCV[b->header.parentSlot];
	ASSERT(before < 500);
	const Entry& lastB = t.leaves[4].entries.back();
	insertEntry(t, 4, 3, lastB.end, keyAfter(lastB.end), 500);
	ASSERT_EQ(b->header.maxCV, 500);
	ASSERT_EQ(parent->maxSubtreesCV[b->header.parentSlot], 500);
	ASSERT_EQ(parent->header.maxCV, 500);
	ASSERT_EQ(root->header.maxCV, 500);
	checkSameEntries(checkTree(t), modelEntries(t));
}

// propagateUpdates with updateSeparator copies the first key of a node, with its CV, into its
// parent's separator, and goes on upwards while the node is a first child.
TEST_CASE("/resolver/TempConflictBTree/PropagateUpdatesSeparator") {
	// Leaves 0..2 under I0, 3..5 under I1. Each case removes the first entry
	// of a leaf and lists the levels whose separator follows the new first
	// key: 1 for the leaf's own, 2 also for its parent's.
	struct Case {
		std::size_t leaf;
		int levels;
	};
	for (const Case c : {Case{0, 2}, Case{1, 1}, Case{3, 2}, Case{5, 1}}) {
		TestTree t;
		addGapLeaves(t, {3, 3, 3, 3, 3, 3});
		t.build(3);
		LeafBlock* leaf = t.leaves[c.leaf].leaf;
		InternalBlock* parent = leaf->parent();
		const KeyRef parentSeparator = separatorOf(&parent->header);
		const Version parentSeparatorCV = separatorCVOf(&parent->header);

		t.nodes().eraseAt(0, leaf);
		std::vector<Entry>& entries = t.leaves[c.leaf].entries;
		entries.erase(entries.begin());
		Access::updateSeparator(t.tree, leaf);

		const Entry& first = entries.front();
		ASSERT_EQ(t.keys().compareKeys(asRef(first.begin), separatorOf(&leaf->header)), 0);
		ASSERT_EQ(separatorCVOf(&leaf->header), first.cv);
		if (c.levels == 2) {
			ASSERT_EQ(t.keys().compareKeys(asRef(first.begin), separatorOf(&parent->header)), 0);
			ASSERT_EQ(separatorCVOf(&parent->header), first.cv);
		} else {
			ASSERT(separatorOf(&parent->header).firstBlock == parentSeparator.firstBlock);
			ASSERT_EQ(separatorOf(&parent->header).keyOffset, parentSeparator.keyOffset);
			ASSERT_EQ(separatorCVOf(&parent->header), parentSeparatorCV);
		}
		checkSameEntries(checkTree(t), modelEntries(t));
	}
}

// The internal version copies the node's first separator; on the root, and
// on a root leaf, there is nothing to update.
TEST_CASE("/resolver/TempConflictBTree/PropagateUpdatesSeparatorOfInternalNode") {
	TestTree t;
	addGapLeaves(t, {3, 3, 3, 3, 3, 3});
	t.build(3);
	InternalBlock* root = rootNode(t);
	auto* i1 = reinterpret_cast<InternalBlock*>(childAt(root, 1));

	// Make I1's first separator the key one above its lower bound, as if its
	// first leaf had lost a first entry there.
	const Bytes k = keyAfter(t.leaves[3].lowerBound);
	const std::optional<KeyRef> key = t.keys().appendKey(
		reinterpret_cast<const std::byte*>(k.data()),
		static_cast<std::uint16_t>(k.size()), 7);
	ASSERT(key.has_value());
	const auto first = i1->header.order[0];
	i1->separators[first] = *key;
	i1->separatorsCV[first] = 7;

	Access::updateSeparator(t.tree, i1);
	ASSERT_EQ(t.keys().compareKeys(asRef(k), separatorOf(&i1->header)), 0);
	ASSERT_EQ(separatorCVOf(&i1->header), 7);

	const KeyRef rootFirst = root->separators[root->header.order[0]];
	Access::updateSeparator(t.tree, root);
	ASSERT(root->separators[root->header.order[0]].firstBlock == rootFirst.firstBlock);

	TestTree single;
	single.addLeaf({}, entriesFrom(10, 2, 1));
	single.build(2);
	Access::updateSeparator(single.tree, single.leaves[0].leaf);
	checkTree(single);
}

// A leaf whose entries have all expired is removed while searching, before
// it can be reached: the search for a key in its range ends after the last
// entry of its left neighbour, and its neighbours are linked.
TEST_CASE("/resolver/TempConflictBTree/FindKeyRemovesExpiredLeaf") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {20, 20, 20}));
	t.addLeaf(key(20), withCVs(entriesFrom(20, 3, 0), {5, 6, 7}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {20, 20, 20}));
	t.build(3);
	InternalBlock* root = rootNode(t);
	Access::setReclaimCV(t.tree, 10);

	LeafBlock* leaf = nullptr;
	std::size_t rank = 999;
	BNodeHeader* nodeNeedsSplit = nullptr;
	ASSERT(!Access::findKey(t.tree, asRef(key(25)), leaf, rank, nodeNeedsSplit));
	ASSERT(leaf == t.leaves[0].leaf);
	ASSERT_EQ(rank, 3);
	ASSERT(nodeNeedsSplit == nullptr);
	ASSERT_EQ(root->header.count, 2);
	ASSERT(t.leaves[0].leaf->right == t.leaves[2].leaf);
	ASSERT(t.leaves[2].leaf->left == t.leaves[0].leaf);
	ASSERT_EQ(t.keys().compareKeys(asRef(key(30)),
								   separatorOf(&t.leaves[2].leaf->header)), 0);
	t.leaves[1].entries.clear();
	checkSameEntries(checkTree(t), modelEntries(t));
}

// The first leaf has expired and is removed by the search, so a read that
// starts below every remaining key lands at the start of the next leaf:
// there is no previous entry, and the ranges to the right are checked.
TEST_CASE("/resolver/TempConflictBTree/HasConflictAfterFirstLeafRemoved") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 2, 0), {5, 6}));
	t.addLeaf(key(20), {{key(20), key(30), 30}});
	t.addLeaf(key(40), {{key(40), key(50), 30}});
	t.build(3);
	Access::setReclaimCV(t.tree, 10);

	ASSERT(t.hasConflict(key(10), key(25), 20));
	ASSERT_EQ(rootNode(t)->header.count, 2);
	ASSERT(t.leaves[1].leaf->left == nullptr);
	ASSERT(!t.hasConflict(key(10), key(25), 30));
	ASSERT(!t.hasConflict(key(10), key(20), 10));
	t.leaves[0].entries.clear();
	checkSameEntries(checkTree(t), modelEntries(t));
}

namespace {

// The entries after writing [begin, end) with cv over entries: covered parts
// are replaced, and the parts of a range outside the write keep its CV.
std::vector<Entry> applyWrite(const std::vector<Entry>& entries,
							  const Bytes& begin, const Bytes& end, Version cv) {
	std::vector<Entry> result;
	for (const Entry& e : entries) {
		if (!(e.begin < end) || !(begin < e.end)) {
			result.push_back(e);
			continue;
		}
		if (e.begin < begin) {
			result.push_back({e.begin, begin, e.cv});
		}
		if (end < e.end) {
			result.push_back({end, e.end, e.cv});
		}
	}
	result.push_back({begin, end, cv});
	std::sort(result.begin(), result.end(),
			  [](const Entry& a, const Entry& b) { return a.begin < b.begin; });
	return result;
}

// Writes [begin, end) with cv and checks the tree, its entries and its
// conflicts against the model, which is updated.
void checkWrite(TestTree& t, std::vector<Entry>& model, const Bytes& begin,
				const Bytes& end, Version cv) {
	model = applyWrite(model, begin, end, cv);
	Access::addWrite(t.tree, asRef(begin), asRef(end), cv);
	checkSameEntries(checkTree(t), model);
}

// Leaves of perLeaf entries [100 + 20 j, 110 + 20 j) with CV 1 + j, over
// wide keys, so that writes can fall inside, across and between ranges.
std::vector<Entry> spacedRanges(TestTree& t, unsigned leaves, unsigned perLeaf,
								std::size_t fanout) {
	unsigned j = 0;
	for (unsigned i = 0; i < leaves; ++i) {
		std::vector<Entry> entries;
		for (unsigned n = 0; n < perLeaf; ++n, ++j) {
			entries.push_back({wideKey(100 + 20 * j), wideKey(110 + 20 * j),
							   1 + static_cast<Version>(j)});
		}
		const Bytes lowerBound = i == 0 ? Bytes{} : entries.front().begin;
		t.addLeaf(lowerBound, std::move(entries));
	}
	t.build(fanout);
	return modelEntries(t);
}

} // namespace

// Each kind of write, alone on a fresh tree of three leaves:
// inside a range, over the start or the end of ranges, with boundaries
// equal to those of ranges, and in a gap.
TEST_CASE("/resolver/TempConflictBTree/AddWriteKinds") {
	struct Case {
		unsigned begin;
		unsigned end;
	};
	const Case cases[] = {
		{103, 107},  // inside a range: both exteriors keep CV 1
		{105, 115},  // left exterior only
		{115, 125},  // right exterior only
		{105, 125},  // left and right exteriors of different ranges
		{105, 165},  // and the ranges between them erased
		{100, 110},  // exactly one range
		{120, 150},  // exactly two ranges and the gap between them
		{110, 120},  // exactly a gap, touching both neighbours
		{112, 118},  // inside a gap
		{100, 105},  // the start of a range
		{105, 110},  // the end of a range
		{90, 95},    // before every range
		{400, 410},  // after every range
		{90, 400},   // over every range
		{275, 285},  // over the end of a leaf and the start of the next
	};
	for (const Case& c : cases) {
		TestTree t;
		std::vector<Entry> model = spacedRanges(t, 3, 3, 3);
		checkWrite(t, model, wideKey(c.begin), wideKey(c.end), 1000);
	}
}

// Many writes in a row, at random places and of random lengths, grow the
// tree through several levels of splits. After each one the structure and
// entries match the model; every 250 writes, and at the end, so do the
// conflicts.
TEST_CASE("/resolver/TempConflictBTree/AddWriteAgainstModel") {
	TestTree t(64 * PageSize);
	std::vector<Entry> model;
	std::uint32_t state = 777;
	auto next = [&state]() {
		state = state * 1103515245u + 12345u;
		return state >> 8;
	};
	for (unsigned n = 0; n < 2000; ++n) {
		const unsigned begin = next() % 30000;
		const unsigned length = n % 50 == 0 ? 1 + next() % 400 : 1 + next() % 8;
		model = applyWrite(model, wideKey(begin), wideKey(begin + length),
						   1000 + n);
		Access::addWrite(t.tree, asRef(wideKey(begin)),
						 asRef(wideKey(begin + length)), 1000 + n);
		// Structure after every write, conflicts now and then, so that a
		// transient error is caught before later writes hide it.
		if (n % 250 == 249) {
			checkSameEntries(checkTree(t), model);
		} else {
			checkSameEntries(checkStructure(t), model);
		}
	}
	checkSameEntries(checkTree(t), model);
	BNodeHeader* root = Access::root(t.tree);
	ASSERT(root->kind == BNodeKind::Internal);
	ASSERT(childAt(reinterpret_cast<InternalBlock*>(root), 0)->kind ==
		   BNodeKind::Internal);
}

// The two writes that showed the need for the invariant, now over a valid
// tree: both erase what they cover and keep the separators in order.
TEST_CASE("/resolver/TempConflictBTree/AddWriteAcrossLeaves") {
	for (unsigned end : {25u, 31u}) {
		TestTree t;
		t.addLeaf({}, entriesFrom(10, 2, 1));
		t.addLeaf(key(30), entriesFrom(30, 2, 1));
		t.build(3);
		std::vector<Entry> model = modelEntries(t);
		checkWrite(t, model, key(11), key(end), 1000);
	}
}

// A write that covers the whole tree erases every leaf and internal node;
// its range ends up alone in a new root leaf.
TEST_CASE("/resolver/TempConflictBTree/AddWriteErasesWholeTree") {
	TestTree t;
	std::vector<Entry> model = spacedRanges(t, 9, 3, 3);
	checkWrite(t, model, wideKey(0), wideKey(60000), 1000);
	BNodeHeader* root = Access::root(t.tree);
	ASSERT(root->kind == BNodeKind::Leaf);
	ASSERT_EQ(root->count, 1);
	ASSERT(root->ageNext == nullptr && root->agePrevious == nullptr);
	checkWrite(t, model, wideKey(10), wideKey(20), 1001);
}

// A write into an empty tree creates the root leaf.
TEST_CASE("/resolver/TempConflictBTree/AddWriteToEmptyTree") {
	TestTree t;
	std::vector<Entry> model;
	checkWrite(t, model, key(10), key(20), 5);
	ASSERT(Access::root(t.tree)->kind == BNodeKind::Leaf);
	checkWrite(t, model, key(15), key(30), 6);
}

// A write ending inside the first range of a leaf, starting in the previous
// leaf, trims that range and is inserted first in the leaf. The separators
// of the leaf and of its parent become the start of the write.
TEST_CASE("/resolver/TempConflictBTree/AddWriteTrimsFirstRangeOfLeaf") {
	TestTree t;
	std::vector<Entry> model = spacedRanges(t, 6, 3, 3);
	LeafBlock* leaf = t.leaves[3].leaf;
	InternalBlock* parent = leaf->parent();
	ASSERT(childAt(parent, 0) == &leaf->header);
	checkWrite(t, model, wideKey(275), wideKey(285), 1000);
	ASSERT(leaf->parent() == parent);
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(275)), separatorOf(&leaf->header)), 0);
	ASSERT_EQ(separatorCVOf(&leaf->header), 1000);
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(275)), separatorOf(&parent->header)), 0);
	ASSERT_EQ(separatorCVOf(&parent->header), 1000);
}

// Trimming the first range of a leaf on its own moves the separators to the
// new start, keeping the range's CV, up while the leaf is a first child.
// Elsewhere, or without a contained end, nothing changes.
TEST_CASE("/resolver/TempConflictBTree/PreserveRightExteriorUpdatesSeparator") {
	TestTree t;
	spacedRanges(t, 6, 3, 3);
	LeafBlock* leaf = t.leaves[3].leaf;
	InternalBlock* parent = leaf->parent();
	const Version cv = t.leaves[3].entries.front().cv;

	Access::preserveRightExterior(t.tree, asRef(wideKey(285)), leaf, 0);
	ASSERT_EQ(t.keys().compareBegin(asRef(wideKey(285)),
									leaf->ranges[leaf->header.order[0]]), 0);
	ASSERT_EQ(leaf->cv[leaf->header.order[0]], cv);
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(285)), separatorOf(&leaf->header)), 0);
	ASSERT_EQ(separatorCVOf(&leaf->header), cv);
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(285)), separatorOf(&parent->header)), 0);
	t.leaves[3].entries.front().begin = wideKey(285);
	checkSameEntries(checkTree(t), modelEntries(t));

	// Not the first entry: only the range changes.
	const KeyRef separator = separatorOf(&leaf->header);
	Access::preserveRightExterior(t.tree, asRef(wideKey(305)), leaf, 1);
	ASSERT(separatorOf(&leaf->header).keyOffset == separator.keyOffset);
	t.leaves[3].entries[1].begin = wideKey(305);
	checkSameEntries(checkTree(t), modelEntries(t));

	// end at or beyond the range's bounds, a null leaf or the end position:
	// nothing to do.
	Access::preserveRightExterior(t.tree, asRef(wideKey(285)), leaf, 0);
	Access::preserveRightExterior(t.tree, asRef(wideKey(290)), leaf, 0);
	Access::preserveRightExterior(t.tree, asRef(wideKey(400)), leaf, 0);
	Access::preserveRightExterior(t.tree, asRef(wideKey(285)), nullptr, 0);
	Access::preserveRightExterior(t.tree, asRef(wideKey(285)), leaf, 3);
	checkSameEntries(checkTree(t), modelEntries(t));
}

namespace {

// Erases the entry at (leaf, rank) and checks the tree and the cursor: it
// must identify the entry that followed the erased one, or be the end
// position of the last leaf, or be null when the tree becomes empty.
void eraseAndCheck(TestTree& t, std::vector<Entry>& model, LeafBlock*& leaf,
				   std::size_t& rank) {
	const RangeRef& range = leaf->ranges[leaf->header.order[rank]];
	std::size_t index = 0;
	while (t.keys().compareBegin(asRef(model[index].begin), range) != 0) {
		++index;
	}
	model.erase(model.begin() + index);
	Access::eraseAt(t.tree, rank, leaf);
	if (model.empty()) {
		ASSERT(leaf == nullptr);
		ASSERT_EQ(rank, 0);
		ASSERT(Access::root(t.tree) == nullptr);
	} else if (index < model.size()) {
		ASSERT(leaf != nullptr && rank < leaf->header.count);
		ASSERT_EQ(t.keys().compareBegin(asRef(model[index].begin),
										leaf->ranges[leaf->header.order[rank]]), 0);
	} else {
		ASSERT(leaf != nullptr && leaf->right == nullptr);
		ASSERT_EQ(rank, leaf->header.count);
	}
	checkSameEntries(checkTree(t), model);
}

} // namespace

// Erasing entries that leave their leaf non-empty: in the middle, first
// (the separators follow, across levels while the leaf is a first child),
// last with a leaf to the right, and last of the last leaf.
TEST_CASE("/resolver/TempConflictBTree/EraseAtKeepsLeaf") {
	struct Case {
		std::size_t leaf;
		std::size_t rank;
	};
	for (const Case c : {Case{1, 1}, Case{3, 0}, Case{4, 0}, Case{0, 0},
						 Case{2, 2}, Case{5, 2}}) {
		TestTree t;
		std::vector<Entry> model = spacedRanges(t, 6, 3, 3);
		LeafBlock* leaf = t.leaves[c.leaf].leaf;
		LeafBlock* const erasedFrom = leaf;
		std::size_t rank = c.rank;
		eraseAndCheck(t, model, leaf, rank);
		if (c.rank == 0) {
			const auto first = erasedFrom->header.order[0];
			ASSERT_EQ(t.keys().compareBegin(
						  [&] {
							  const KeyRef k = separatorOf(&erasedFrom->header);
							  (void)k;
							  return asRef(model[3 * c.leaf].begin);
						  }(),
						  erasedFrom->ranges[first]), 0);
			if (c.leaf != 0) {
				ASSERT_EQ(t.keys().compareKeys(asRef(model[3 * c.leaf].begin),
											   separatorOf(&erasedFrom->header)), 0);
				ASSERT_EQ(separatorCVOf(&erasedFrom->header), model[3 * c.leaf].cv);
			}
		}
	}
}

// A leaf that stops needing a split by an erasure is no longer counted, up
// through a parent with a single free slot.
TEST_CASE("/resolver/TempConflictBTree/EraseAtPropagatesSplitCleared") {
	TestTree t;
	InternalBlock* i0 = fullLeafUnderCriticalNode(t, LeafCapacity - 1);
	InternalBlock* root = rootNode(t);
	ASSERT_EQ(i0->header.childrenNeedingSplit, 1);
	std::vector<Entry> model = modelEntries(t);
	LeafBlock* leaf = t.leaves[1].leaf;
	std::size_t rank = 5;
	eraseAndCheck(t, model, leaf, rank);
	ASSERT_EQ(i0->header.childrenNeedingSplit, 0);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
}

// A full leaf still needs a split after losing one entry: the counts above
// do not change.
TEST_CASE("/resolver/TempConflictBTree/EraseAtKeepsSplitOfFullLeaf") {
	TestTree t;
	InternalBlock* i0 = fullLeafUnderCriticalNode(t);
	InternalBlock* root = rootNode(t);
	std::vector<Entry> model = modelEntries(t);
	LeafBlock* leaf = t.leaves[1].leaf;
	std::size_t rank = 5;
	eraseAndCheck(t, model, leaf, rank);
	ASSERT(Access::needsSplit(t.leaves[1].leaf));
	ASSERT_EQ(i0->header.childrenNeedingSplit, 1);
	ASSERT_EQ(root->header.childrenNeedingSplit, 1);
}

// Erasing the only entry of a leaf removes it: its neighbours are linked,
// the parent loses it, and the cursor moves to the next leaf, or to the end
// of the previous one when it was the last leaf. When it was a first child,
// the separators above follow the parent's new first key.
TEST_CASE("/resolver/TempConflictBTree/EraseAtRemovesLeaf") {
	for (std::size_t i : {std::size_t{1}, std::size_t{3}, std::size_t{5}, std::size_t{0}}) {
		TestTree t;
		std::vector<Entry> model = spacedRanges(t, 6, 1, 3);
		LeafBlock* leaf = t.leaves[i].leaf;
		InternalBlock* parent = leaf->parent();
		const std::size_t count = parent->header.count;
		std::size_t rank = 0;
		eraseAndCheck(t, model, leaf, rank);
		ASSERT_EQ(parent->header.count, count - 1);
		if (i + 1 < t.leaves.size()) {
			ASSERT(leaf == t.leaves[i + 1].leaf);
			ASSERT(leaf->left == (i == 0 ? nullptr : t.leaves[i - 1].leaf));
		} else {
			ASSERT(leaf == t.leaves[i - 1].leaf);
			ASSERT(leaf->right == nullptr);
		}
		if (i == 3) {
			ASSERT_EQ(t.keys().compareKeys(asRef(model[3].begin),
										   separatorOf(&parent->header)), 0);
			ASSERT_EQ(separatorCVOf(&parent->header), model[3].cv);
		}
	}
}

// Removing the last leaf of an internal node removes the node too, and a
// root left with a single child is replaced by it, down to a leaf.
TEST_CASE("/resolver/TempConflictBTree/EraseAtRemovesEmptyParents") {
	// root -> [I0 -> [L0, L1], I1 -> [L2, L3]], one entry per leaf.
	TestTree t;
	std::vector<Entry> model = spacedRanges(t, 4, 1, 2);
	InternalBlock* root = rootNode(t);
	ASSERT_EQ(root->header.count, 2);
	LeafBlock* leaf = t.leaves[3].leaf;
	std::size_t rank = 0;
	eraseAndCheck(t, model, leaf, rank);
	leaf = t.leaves[2].leaf;
	rank = 0;
	// I1 is left empty and removed; the root, with I0 alone, is replaced by
	// I0, which still has two leaves.
	eraseAndCheck(t, model, leaf, rank);
	BNodeHeader* newRoot = Access::root(t.tree);
	ASSERT(newRoot->kind == BNodeKind::Internal);
	ASSERT_EQ(newRoot->count, 2);
	ASSERT(newRoot->parent == nullptr);
	leaf = t.leaves[1].leaf;
	rank = 0;
	// I0 with L0 alone is replaced by L0.
	eraseAndCheck(t, model, leaf, rank);
	ASSERT(Access::root(t.tree) == &t.leaves[0].leaf->header);
	ASSERT(t.leaves[0].leaf->parent() == nullptr);
	ASSERT(t.nodes().oldestNode() == &t.leaves[0].leaf->header);
	ASSERT(t.leaves[0].leaf->header.ageNext == nullptr);
	leaf = t.leaves[0].leaf;
	rank = 0;
	eraseAndCheck(t, model, leaf, rank);
	ASSERT(t.nodes().oldestNode() == nullptr);
}

// The only leaf left, under a chain of internal nodes with one child each
// (refreshing does not shorten the tree), is erased with all of them.
TEST_CASE("/resolver/TempConflictBTree/EraseAtRemovesChainToRoot") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 1, 0), {20}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 1, 0), {5}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 1, 0), {5}));
	t.addLeaf(key(70), withCVs(entriesFrom(70, 1, 0), {5}));
	t.build(2);
	InternalBlock* root = rootNode(t);
	auto* i0 = reinterpret_cast<InternalBlock*>(childAt(root, 0));
	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparators(t.tree, root));
	ASSERT(Access::refreshSeparators(t.tree, i0));
	ASSERT_EQ(root->header.count, 1);
	ASSERT_EQ(i0->header.count, 1);
	std::vector<Entry> model{t.leaves[0].entries.front()};
	LeafBlock* leaf = t.leaves[0].leaf;
	std::size_t rank = 0;
	eraseAndCheck(t, model, leaf, rank);
	ASSERT(t.nodes().oldestNode() == nullptr);
}


// An internal node that needed a split on the way down stops needing it when
// cleaning the leaf below frees slots: findKey then returns no candidate.
TEST_CASE("/resolver/TempConflictBTree/FindKeyDropsCandidateClearedBelow") {
	TestTree t;
	InternalBlock* i0 = fullLeafUnderCriticalNode(t);
	InternalBlock* root = rootNode(t);
	LeafBlock* leaf = t.leaves[1].leaf;
	// The first entry is newer than the others, and so is the separator, so
	// that only the leaf's own cleaning removes them.
	leaf->cv[leaf->header.order[0]] = 100;
	i0->separatorsCV[leaf->header.parentSlot] = 100;
	t.leaves[1].entries.front().cv = 100;
	Access::setReclaimCV(t.tree, 50);

	const Position p = find(t, t.leaves[1].lowerBound);
	ASSERT(p.leaf == leaf);
	ASSERT(p.nodeNeedsSplit == nullptr);
	ASSERT_EQ(leaf->header.count, LeafCapacity - 49);
	ASSERT_EQ(i0->header.childrenNeedingSplit, 0);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	std::vector<Entry>& entries = t.leaves[1].entries;
	entries.erase(std::remove_if(entries.begin(), entries.end(),
								 [](const Entry& e) { return e.cv <= 50; }),
				  entries.end());
	checkSameEntries(checkTree(t), modelEntries(t));
}

namespace {

void collectLeaves(BNodeHeader* node, std::vector<LeafBlock*>& leaves) {
	if (node->kind == BNodeKind::Leaf) {
		leaves.push_back(reinterpret_cast<LeafBlock*>(node));
		return;
	}
	auto* internal = reinterpret_cast<InternalBlock*>(node);
	for (std::size_t r = 0; r < internal->header.count; ++r) {
		collectLeaves(childAt(internal, r), leaves);
	}
}

// Detaches the child at rank of node and checks the result: the subtree is
// returned without parent, its leaves still chained among themselves and
// out of the tree's chain, and the tree holds every other entry.
BNodeHeader* detachAndCheck(TestTree& t, InternalBlock* node, std::size_t rank) {
	BNodeHeader* expected = childAt(node, rank);
	std::vector<LeafBlock*> detached;
	collectLeaves(expected, detached);
	BNodeHeader* subtree = Access::detach(t.tree, rank, node);
	ASSERT(subtree == expected);
	ASSERT(subtree->parent == nullptr);
	ASSERT(detached.front()->left == nullptr);
	ASSERT(detached.back()->right == nullptr);
	for (std::size_t i = 0; i + 1 < detached.size(); ++i) {
		ASSERT(detached[i]->right == detached[i + 1]);
		ASSERT(detached[i + 1]->left == detached[i]);
	}
	for (LeafModel& l : t.leaves) {
		if (std::find(detached.begin(), detached.end(), l.leaf) != detached.end()) {
			l.entries.clear();
		}
	}
	checkSameEntries(checkTree(t), modelEntries(t));
	return subtree;
}

} // namespace

// Detaching a leaf in the middle of its parent links its neighbours.
TEST_CASE("/resolver/TempConflictBTree/DetachLeaf") {
	TestTree t;
	spacedRanges(t, 6, 2, 3);
	InternalBlock* i0 = t.leaves[1].leaf->parent();
	detachAndCheck(t, i0, 1);
	ASSERT_EQ(i0->header.count, 2);
	ASSERT(t.leaves[0].leaf->right == t.leaves[2].leaf);
	ASSERT(t.leaves[2].leaf->left == t.leaves[0].leaf);
}

// Detaching an internal node takes its whole run of leaves out of the chain.
TEST_CASE("/resolver/TempConflictBTree/DetachInternalNode") {
	TestTree t;
	spacedRanges(t, 9, 1, 3);
	InternalBlock* root = rootNode(t);
	ASSERT_EQ(root->header.count, 3);
	BNodeHeader* subtree = detachAndCheck(t, root, 1);
	ASSERT(subtree->kind == BNodeKind::Internal);
	ASSERT_EQ(root->header.count, 2);
	ASSERT(t.leaves[2].leaf->right == t.leaves[6].leaf);
	ASSERT(t.leaves[6].leaf->left == t.leaves[2].leaf);
}

// Detaching a first child changes the separators above while the node is a
// first child itself.
TEST_CASE("/resolver/TempConflictBTree/DetachFirstChildUpdatesSeparators") {
	// root -> [J0, J1, J2], J -> 3 internal nodes, each with 3 leaves.
	TestTree t;
	std::vector<Entry> model = spacedRanges(t, 27, 1, 3);
	LeafBlock* leaf = t.leaves[9].leaf;
	InternalBlock* parent = leaf->parent();
	InternalBlock* grandparent = parent->parent();
	ASSERT(childAt(parent, 0) == &leaf->header);
	ASSERT(childAt(grandparent, 0) == &parent->header);
	detachAndCheck(t, parent, 0);
	ASSERT_EQ(t.keys().compareKeys(asRef(model[10].begin), separatorOf(&parent->header)), 0);
	ASSERT_EQ(separatorCVOf(&parent->header), model[10].cv);
	ASSERT_EQ(t.keys().compareKeys(asRef(model[10].begin), separatorOf(&grandparent->header)), 0);
	ASSERT_EQ(separatorCVOf(&grandparent->header), model[10].cv);
}

// The parent no longer counts a detached child that needed a split, and a
// parent that needed one stops when it gets a slot back.
TEST_CASE("/resolver/TempConflictBTree/DetachUpdatesSplitCounts") {
	for (std::size_t rank : {std::size_t{1}, std::size_t{2}}) {
		TestTree t;
		InternalBlock* i0 = fullLeafUnderCriticalNode(t);
		InternalBlock* root = rootNode(t);
		detachAndCheck(t, i0, rank);
		ASSERT_EQ(i0->header.childrenNeedingSplit, rank == 1 ? 0 : 1);
		ASSERT(!Access::needsSplit(i0));
		ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	}
}

// Detaching the last child of an internal node removes the node, and a root
// left with one child is replaced by it.
TEST_CASE("/resolver/TempConflictBTree/DetachRemovesEmptyParents") {
	// root -> [I0 -> [L0, L1], I1 -> [L2, L3]].
	TestTree t;
	spacedRanges(t, 4, 1, 2);
	InternalBlock* i0 = t.leaves[0].leaf->parent();
	InternalBlock* i1 = t.leaves[2].leaf->parent();
	detachAndCheck(t, i1, 0);
	ASSERT_EQ(i1->header.count, 1);
	detachAndCheck(t, i1, 0);
	ASSERT(Access::root(t.tree) == &i0->header);
	ASSERT(i0->parent() == nullptr);
	ASSERT(t.leaves[1].leaf->right == nullptr);
	// A root with two leaves, after detaching one, is the other leaf.
	detachAndCheck(t, i0, 1);
	ASSERT(Access::root(t.tree) == &t.leaves[0].leaf->header);
	ASSERT(t.leaves[0].leaf->parent() == nullptr);
}

// Detaching the only leaf, under a chain of internal nodes with one child
// each, empties the tree.
TEST_CASE("/resolver/TempConflictBTree/DetachEmptiesTree") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 1, 0), {20}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 1, 0), {5}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 1, 0), {5}));
	t.addLeaf(key(70), withCVs(entriesFrom(70, 1, 0), {5}));
	t.build(2);
	InternalBlock* root = rootNode(t);
	auto* i0 = reinterpret_cast<InternalBlock*>(childAt(root, 0));
	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparators(t.tree, root));
	ASSERT(Access::refreshSeparators(t.tree, i0));
	ASSERT_EQ(i0->header.count, 1);
	for (std::size_t i = 1; i < 4; ++i) {
		t.leaves[i].entries.clear();
	}
	detachAndCheck(t, i0, 0);
	ASSERT(Access::root(t.tree) == nullptr);
	ASSERT(t.nodes().oldestNode() == &t.leaves[0].leaf->header);
}

