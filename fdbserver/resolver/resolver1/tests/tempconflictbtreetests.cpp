#include "test.h"
#include "tempconflictbtree.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
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
	static bool refreshSeparatorDeferred(TempConflictBTree& t,
										 InternalBlock* node, std::size_t rank) {
		return t.refreshSeparatorDeferred(node, rank);
	}
	static std::optional<std::pair<const RangeRef*, Version>> firstLiveKey(
		TempConflictBTree& t, BNodeHeader* node) {
		return t.firstLiveKey(node);
	}
	static constexpr Version EmptyChildCV = TempConflictBTree::EmptyChildCV;
	static std::size_t pendingCount(TempConflictBTree& t) {
		return t.pending_.size();
	}
	static void cleanPending(TempConflictBTree& t) {
		t.cleanPending();
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
	static bool reclaimOldest(TempConflictBTree& t) {
		return t.reclaimOldest();
	}
	static bool reclaimUntilAvail(TempConflictBTree& t, std::size_t n) {
		return t.reclaimUntilAvail(n);
	}
	static void removeSubtree(TempConflictBTree& t, BNodeHeader* node) {
		t.removeSubtree(node);
	}
	static PagePool& pool(TempConflictBTree& t) { return t.pool_; }
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

// Whether two free-slot maps are equal, whatever their number of words.
template<std::size_t C>
bool sameMap(const FreeSlots<C>& a, const FreeSlots<C>& b) {
	return std::equal(std::begin(a.words), std::end(a.words), std::begin(b.words));
}

// A map with exactly the given slots free.
template<std::size_t C>
FreeSlots<C> mapWithFree(std::initializer_list<std::size_t> free) {
	FreeSlots<C> map;
	for (std::uint64_t& w : map.words) {
		w = 0;
	}
	for (std::size_t slot : free) {
		map.setFree(slot);
	}
	return map;
}

// A map with every slot free but the first used ones.
template<std::size_t C>
FreeSlots<C> mapWithFirstUsed(std::size_t used) {
	FreeSlots<C> map;
	for (std::size_t slot = 0; slot < used; ++slot) {
		map.unsetFree(slot);
	}
	return map;
}

// A two-level layout whose first internal node needs a split: it has
// CriticalChildren leaves, the last CriticalFull of them full, so that
// children plus children needing a split fill InternalCapacity. Five more
// leaves follow, under a second internal node. At 2 KiB: 25 leaves, 15 full.
constexpr std::size_t CriticalChildren = InternalCapacity * 5 / 8;
constexpr std::size_t CriticalFull = InternalCapacity - CriticalChildren;
static_assert(CriticalFull <= CriticalChildren);

std::vector<bool> criticalLayout() {
	std::vector<bool> full(CriticalChildren + 5, false);
	for (std::size_t i = CriticalChildren - CriticalFull; i < CriticalChildren; ++i) {
		full[i] = true;
	}
	return full;
}

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
		return reinterpret_cast<const LeafBlock*>(node)->freeSlotCount() <= 1;
	}
	return node->childrenNeedingSplit ==
		reinterpret_cast<const InternalBlock*>(node)->freeSlotCount();
}

std::uint8_t childrenNeedingSplit(const InternalBlock* node) {
	std::uint8_t count = 0;
	for (std::size_t r = 0; r < node->header.count; ++r) {
		count += needsSplit(node->children[node->order[r]]);
	}
	return count;
}

// A tree built directly from nodes. Entries are inserted in reverse order,
// always at rank 0, so that order is never the identity.
class TestTree {
public:
	// The default budget is 16 pages, or 512 blocks if that is more.
	explicit TestTree(std::size_t budget = std::max<std::size_t>(16 * PageSize, 512 * BlockSize))
	  : tree(budget) {}

	TempConflictBTree tree;
	std::vector<LeafModel> leaves;
	// Newest maximum CV of the nodes created, since nodes must be created
	// in nondecreasing CV order and inserting an entry raises a leaf's CV.
	Version newestCV = 0;

	KeyStore& keys() { return Access::keys(tree); }
	BNodeStore& nodes() { return Access::nodes(tree); }

	LeafBlock* addLeaf(const Bytes& lowerBound, std::vector<Entry> entries) {
		std::vector<RangeRef> ranges;
		for (auto e = entries.rbegin(); e != entries.rend(); ++e) {
			const std::optional<RangeRef> range =
				keys().appendRange(asRef(e->begin), asRef(e->end), e->cv);
			ASSERT(range.has_value());
			ranges.insert(ranges.begin(), *range);
		}
		return addLeaf(lowerBound, std::move(entries), ranges);
	}

	// As above, with the entries' ranges already stored, so that a test can
	// choose the order of the records in the key store.
	LeafBlock* addLeaf(const Bytes& lowerBound, std::vector<Entry> entries,
					   const std::vector<RangeRef>& ranges) {
		ASSERT_EQ(entries.size(), ranges.size());
		LeafBlock* leaf = nodes().allocateLeaf(newestCV);
		ASSERT(leaf != nullptr);

		for (std::size_t i = entries.size(); i > 0; --i) {
			nodes().insertAt(0, ranges[i - 1], entries[i - 1].cv, leaf);
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
					const std::optional<RangeRef> key = keys().appendKey(
						reinterpret_cast<const std::byte*>(separator.data()),
						static_cast<std::uint16_t>(separator.size()),
						level[j - 1].cv);
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
	ASSERT(root->children[root->order[0]]->kind == BNodeKind::Internal);

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
		ASSERT_EQ(slot, leaf->order[rank]);
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
		ASSERT_EQ(slot, leaf->order[rank]);
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
	ASSERT_EQ(slot, leaf->order[1]);

	leaf = t.leaves[3].leaf;
	rank = 0;
	ASSERT(Access::nextRank(t.tree, leaf, rank, slot));
	ASSERT(leaf == t.leaves[4].leaf);
	ASSERT_EQ(rank, 0);
	ASSERT_EQ(slot, leaf->order[0]);

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
// Entries whose key block has been reclaimed are skipped without reading
// them; they must have expired.
std::vector<Entry> leafEntries(TestTree& t, const LeafBlock* leaf) {
	std::vector<Entry> entries;
	for (std::size_t r = 0; r < leaf->header.count; ++r) {
		const auto slot = leaf->order[r];
		const RangeRef& range = leaf->ranges[slot];
		if (!t.keys().isValid(range)) {
			ASSERT(leaf->cv[slot] <= t.tree.reclaimCV());
			continue;
		}
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
		const auto slot = internal->order[r];
		ASSERT(internal->children[slot]->parent == internal);
		ASSERT_EQ(internal->children[slot]->parentSlot, slot);
		ASSERT(internal->maxSubtreesCV[slot] >= internal->children[slot]->maxCV);
		ASSERT(node->maxCV >= internal->children[slot]->maxCV);
		const std::size_t before = entries.size();
		checkSubtree(t, internal->children[slot], leaves, entries);

		// Every begin in this child is at least its separator, and every
		// begin before it is below it. Entries are checked to be in order,
		// so the entries on either side of the separator are enough.
		const RangeRef& separator = internal->separators[slot];
		// A separator whose key was reclaimed has expired and is not read.
		if (!t.keys().isKeyValid(separator)) {
			ASSERT(internal->separatorsCV[slot] <= t.tree.reclaimCV());
			continue;
		}
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
	return node->children[node->order[rank]];
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
	FreeSlots<LeafCapacity> expectedFree;
	expectedFree.unsetFree(leaf->order[0]);
	expectedFree.unsetFree(leaf->order[1]);
	ASSERT(sameMap(leaf->freeSlots, expectedFree));

	ASSERT(!Access::cleanExpired(t.tree, leaf));
	checkTree(t);
}

TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorKeepsFreshSeparator") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {5, 6, 7}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {8, 2, 9}));
	t.build(3);
	InternalBlock* root = rootNode(t);
	const RangeRef before = root->separators[root->order[1]];

	Access::setReclaimCV(t.tree, 7);
	ASSERT(!Access::refreshSeparator(t.tree, root, 1));
	ASSERT(root->separators[root->order[1]].firstBlock == before.firstBlock);
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

	const auto slot = root->order[1];
	ASSERT_EQ(root->header.count, 3);
	ASSERT_EQ(root->separatorsCV[slot], 30);
	ASSERT_EQ(t.keys().compareKeys(asRef(key(34)), root->separators[slot]), 0);
	ASSERT(t.keys().isKeyValid(root->separators[slot]));
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
	ASSERT(!t.leaves[1].leaf->hasSpace());
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

Bytes wideKey(unsigned value);
std::vector<Entry> wideEntriesFrom(unsigned first, unsigned count, Version cv);

// root -> [I0, I1]. I0 has a single free slot and its second child is a leaf
// with entries CVs 1..entries, full by default, so I0 needs a split and the
// root counts it when that leaf needs one. The other leaves hold one entry
// each, with CV 100. Keys are wide, so that any capacity fits.
InternalBlock* fullLeafUnderCriticalNode(TestTree& t,
										 unsigned entries = LeafCapacity) {
	constexpr std::size_t children = InternalCapacity - 1;
	t.addLeaf({}, wideEntriesFrom(10, 1, 100));
	t.addLeaf(wideKey(12), wideEntriesFrom(12, entries, 1));
	unsigned first = 12 + 2 * entries;
	for (std::size_t i = 2; i <= children; ++i, first += 2) {
		t.addLeaf(wideKey(first), wideEntriesFrom(first, 1, 100));
	}
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

	ASSERT_EQ(t.leaves[1].leaf->freeSlotCount(), 2);
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
	const auto slot = root->order[1];
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
		ASSERT(root->separatorsCV[root->order[r]] > 10);
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
	ASSERT(sameMap(root->freeSlots, FreeSlots<InternalCapacity>()));
}

namespace {

// Same record, ignoring the end of a range: a separator copied from another.
bool sameKeyRecord(const RangeRef& a, const RangeRef& b) {
	return a.firstBlock == b.firstBlock && a.keyOffset == b.keyOffset &&
		   a.beginLength == b.beginLength && a.blockGen == b.blockGen;
}

Version separatorCV(const InternalBlock* node, std::size_t rank) {
	return node->separatorsCV[node->order[rank]];
}

const RangeRef& separatorAt(const InternalBlock* node, std::size_t rank) {
	return node->separators[node->order[rank]];
}

} // namespace

// The expired separator becomes the first live entry of the leaf, which
// keeps its expired entries until the queue is cleaned.
TEST_CASE("/resolver/TempConflictBTree/DeferredRefreshKeepsExpiredEntries") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {20, 20, 20}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 4, 0), {5, 10, 30, 8}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 3, 0), {20, 20, 20}));
	t.build(3);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparatorDeferred(t.tree, root, 1));

	ASSERT_EQ(separatorCV(root, 1), 30);
	ASSERT_EQ(t.keys().compareKeys(asRef(key(34)), separatorAt(root, 1)), 0);
	ASSERT_EQ(t.leaves[1].leaf->header.count, 4);
	ASSERT_EQ(Access::pendingCount(t.tree), 1);
	// Refreshed, it is not visited again.
	ASSERT(Access::refreshSeparatorDeferred(t.tree, root, 1));
	ASSERT_EQ(Access::pendingCount(t.tree), 1);

	Access::cleanPending(t.tree);
	ASSERT_EQ(Access::pendingCount(t.tree), 0);
	ASSERT_EQ(leafEntries(t, t.leaves[1].leaf).size(), 1);
	checkTree(t);
}

// A leaf without live entries stays in the tree, marked with the separator
// of the child before it, until the queue is cleaned.
TEST_CASE("/resolver/TempConflictBTree/DeferredRefreshMarksEmptyLeaf") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {20, 20, 20}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {5, 6, 7}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 3, 0), {20, 20, 20}));
	t.build(3);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 10);
	ASSERT(!Access::refreshSeparatorDeferred(t.tree, root, 1));

	ASSERT_EQ(root->header.count, 3);
	ASSERT_EQ(separatorCV(root, 1), Access::EmptyChildCV);
	ASSERT(sameKeyRecord(separatorAt(root, 1), separatorAt(root, 0)));
	ASSERT_EQ(t.leaves[1].leaf->header.count, 3);
	ASSERT(t.leaves[0].leaf->right == t.leaves[1].leaf);

	// Marked, it is not visited again.
	ASSERT(!Access::refreshSeparatorDeferred(t.tree, root, 1));
	ASSERT_EQ(Access::pendingCount(t.tree), 1);

	Access::cleanPending(t.tree);
	ASSERT_EQ(root->header.count, 2);
	ASSERT(t.leaves[0].leaf->right == t.leaves[2].leaf);
	checkTree(t);
}

// Leading children without live entries take the separator of the first
// live child after them.
TEST_CASE("/resolver/TempConflictBTree/DeferredRefreshMarksLeadingChildren") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 2, 0), {1, 2}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 2, 0), {3, 4}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 2, 0), {5, 20}));
	t.addLeaf(key(70), withCVs(entriesFrom(70, 2, 0), {20, 20}));
	t.build(4);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 10);
	ASSERT(!Access::refreshSeparatorDeferred(t.tree, root, 0));

	ASSERT_EQ(separatorCV(root, 2), 20);
	ASSERT_EQ(t.keys().compareKeys(asRef(key(52)), separatorAt(root, 2)), 0);
	for (std::size_t rank = 0; rank < 2; ++rank) {
		ASSERT_EQ(separatorCV(root, rank), Access::EmptyChildCV);
		ASSERT(sameKeyRecord(separatorAt(root, rank), separatorAt(root, 2)));
	}
	ASSERT_EQ(root->header.count, 4);
	ASSERT_EQ(Access::pendingCount(t.tree), 3);

	Access::cleanPending(t.tree);
	ASSERT_EQ(root->header.count, 2);
	ASSERT_EQ(leafEntries(t, t.leaves[2].leaf).size(), 1);
	checkTree(t);
}

// The separator is refreshed down through an internal child, whose own
// leading children without live entries are marked.
TEST_CASE("/resolver/TempConflictBTree/DeferredRefreshThroughInternalChild") {
	TestTree t;
	threeLevels(t, 5, 8, 40);
	InternalBlock* root = rootNode(t);
	auto* i1 = reinterpret_cast<InternalBlock*>(childAt(root, 1));

	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparatorDeferred(t.tree, root, 1));

	ASSERT_EQ(separatorCV(root, 1), 40);
	ASSERT_EQ(t.keys().compareKeys(asRef(key(110)), separatorAt(root, 1)), 0);
	ASSERT_EQ(i1->header.count, 3);
	ASSERT_EQ(separatorCV(i1, 2), 40);
	for (std::size_t rank = 0; rank < 2; ++rank) {
		ASSERT_EQ(separatorCV(i1, rank), Access::EmptyChildCV);
		ASSERT(sameKeyRecord(separatorAt(i1, rank), separatorAt(i1, 2)));
	}
	ASSERT_EQ(Access::pendingCount(t.tree), 2);

	Access::cleanPending(t.tree);
	ASSERT_EQ(i1->header.count, 1);
	ASSERT(childAt(i1, 0) == &t.leaves[5].leaf->header);
	ASSERT(t.leaves[2].leaf->right == t.leaves[5].leaf);
	checkTree(t);
}

// An internal child without live entries is marked; cleaning the queue
// removes its leaves and then the child itself.
TEST_CASE("/resolver/TempConflictBTree/DeferredRefreshMarksEmptyInternalChild") {
	TestTree t;
	threeLevels(t, 5, 8, 9);
	InternalBlock* root = rootNode(t);

	Access::setReclaimCV(t.tree, 10);
	ASSERT(!Access::refreshSeparatorDeferred(t.tree, root, 1));

	ASSERT_EQ(root->header.count, 2);
	ASSERT_EQ(separatorCV(root, 1), Access::EmptyChildCV);
	ASSERT(sameKeyRecord(separatorAt(root, 1), separatorAt(root, 0)));
	ASSERT_EQ(Access::pendingCount(t.tree), 3);

	Access::cleanPending(t.tree);
	ASSERT_EQ(root->header.count, 1);
	ASSERT(t.leaves[2].leaf->right == nullptr);
	checkTree(t);
}

// firstLiveKey gives the smallest live key under a node, and nothing when
// every entry has expired, in which case cleaning empties the tree.
TEST_CASE("/resolver/TempConflictBTree/FirstLiveKey") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 2, 0), {1, 2}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 2, 0), {3, 15}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 2, 0), {5, 6}));
	t.build(3);
	BNodeHeader* root = Access::root(t.tree);

	Access::setReclaimCV(t.tree, 10);
	const auto first = Access::firstLiveKey(t.tree, root);
	ASSERT(first.has_value());
	ASSERT_EQ(first->second, 15);
	ASSERT_EQ(t.keys().compareKeys(asRef(key(32)), *first->first), 0);
	Access::cleanPending(t.tree);
	checkTree(t);

	Access::setReclaimCV(t.tree, 20);
	ASSERT(!Access::firstLiveKey(t.tree, Access::root(t.tree)).has_value());
	Access::cleanPending(t.tree);
	ASSERT(Access::root(t.tree) == nullptr);
}

// A leaf queued twice is cleaned once: once released, its cleared
// generation tells the second entry apart.
TEST_CASE("/resolver/TempConflictBTree/CleanPendingSkipsReleasedLeaf") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 2, 0), {20, 20}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 2, 0), {3, 4}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 2, 0), {20, 20}));
	t.build(3);
	InternalBlock* root = rootNode(t);
	BNodeHeader* leaf = &t.leaves[1].leaf->header;

	Access::setReclaimCV(t.tree, 10);
	ASSERT(!Access::firstLiveKey(t.tree, leaf).has_value());
	ASSERT(!Access::firstLiveKey(t.tree, leaf).has_value());
	ASSERT_EQ(Access::pendingCount(t.tree), 2);

	Access::cleanPending(t.tree);
	ASSERT_EQ(root->header.count, 2);
	ASSERT(t.leaves[0].leaf->right == t.leaves[2].leaf);
	checkTree(t);
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
		remaining -= needsSplit(node->children[node->order[cut]]) ? 2 : 1;
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

	std::vector<FreeSlots<LeafCapacity>> leafPatterns{
		mapWithFree<LeafCapacity>({0}), mapWithFree<LeafCapacity>({LeafCapacity - 1}),
		mapWithFree<LeafCapacity>({0, LeafCapacity - 1})};
	for (std::size_t used = 0; used <= LeafCapacity; ++used) {
		leafPatterns.push_back(mapWithFirstUsed<LeafCapacity>(used));
	}
	for (const FreeSlots<LeafCapacity>& freeSlots : leafPatterns) {
		leaf->freeSlots = freeSlots;
		const bool expected = freeSlots.count() <= 1;
		ASSERT_EQ(Access::needsSplit(leaf), expected);
		ASSERT_EQ(Access::needsSplit(&leaf->header), expected);
	}

	for (std::size_t used = 0; used <= InternalCapacity; ++used) {
		internal->freeSlots = mapWithFirstUsed<InternalCapacity>(used);
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
	// root -> [I0, I1]; I0 has CriticalChildren leaves, the last CriticalFull
	// full, so it needs a split; I1 has 5 leaves and does not.
	const std::vector<bool> full = criticalLayout();
	TestTree t;
	addWideLeaves(t, full);
	t.build(CriticalChildren);
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
							  : i < CriticalChildren ? i0
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
	const auto slot = root->order[1];
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
	ASSERT(!root->hasSpace());
	ASSERT(Access::needsSplit(root));
	ASSERT(find(t, t.leaves[5].lowerBound).nodeNeedsSplit == &root->header);
	checkSameEntries(checkTree(t), modelEntries(t));
}

// An internal root needing a split is cut by weight: each child needing a
// split counts twice. Both halves stop needing a split, every moved child
// points to the new node, and leaf and rank are left unchanged.
TEST_CASE("/resolver/TempConflictBTree/SplitRootInternal") {
	// Every layout has children plus children needing a split equal to
	// InternalCapacity. At 2 KiB: 40 children; 30 with 10 counted; 20, all.
	const std::size_t n = InternalCapacity, c = n * 3 / 4, k = n - c;
	std::vector<std::vector<bool>> layouts;
	layouts.push_back(std::vector<bool>(n, false));       // full, no child counted
	std::vector<bool> layout(c, false);
	std::fill(layout.begin(), layout.begin() + k, true);
	layouts.push_back(layout);                            // counted children first
	std::reverse(layout.begin(), layout.end());
	layouts.push_back(layout);                            // counted children last
	layout.assign(c, false);
	for (std::size_t i = 0; i < k; ++i) {
		layout[i * (c / k)] = true;
	}
	layouts.push_back(layout);                            // spread
	// All counted when the capacity is even; all but one otherwise.
	layout.assign((n + 1) / 2, true);
	if (n % 2 != 0) {
		layout.back() = false;
	}
	layouts.push_back(layout);

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
			ASSERT_EQ(sibling->order[r], r);
		}
		ASSERT(sameMap(sibling->freeSlots, mapWithFirstUsed<InternalCapacity>(sibling->header.count)));
		checkSameEntries(checkTree(t), modelEntries(t));
	}
}

// An internal node under a parent: the sibling goes right after it in the
// parent, the parent stops counting it, and the sibling's CV reaches the
// root.
TEST_CASE("/resolver/TempConflictBTree/SplitInternalUnderParent") {
	const std::vector<bool> full = criticalLayout();
	TestTree t;
	addWideLeaves(t, full);
	t.build(CriticalChildren);
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
	ASSERT_EQ(sibling->header.count, CriticalChildren - cut);
	ASSERT_EQ(old->header.childrenNeedingSplit + sibling->header.childrenNeedingSplit, CriticalFull);
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
		ASSERT_EQ(leaf->freeSlotCount(), LeafCapacity - counts[i]);
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
	const auto slot = leaf->order[rank];
	ASSERT_EQ(leaf->cv[slot], cv);
	ASSERT_EQ(t.keys().compareBegin(asRef(begin), leaf->ranges[slot]), 0);

	std::vector<Entry>& entries = t.leaves[i].entries;
	entries.insert(entries.begin() + rank, {begin, end, cv});
	t.newestCV = std::max(t.newestCV, cv);
}

// The separator of node in its parent.
const RangeRef& separatorOf(const BNodeHeader* node) {
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
	ASSERT(!leaf->hasSpace());
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
	const unsigned last = 10 + 2 * LeafCapacity + 10;
	t.addLeaf(wideKey(last), wideEntriesFrom(last, 2, 1));
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
		const RangeRef separator = separatorOf(&leaf->header);
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
		const RangeRef parentSeparator = separatorOf(&parent->header);
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
	ASSERT(!t.leaves[1].leaf->hasSpace());
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
		const RangeRef parentSeparator = separatorOf(&parent->header);
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
	const std::optional<RangeRef> key = t.keys().appendKey(
		reinterpret_cast<const std::byte*>(k.data()),
		static_cast<std::uint16_t>(k.size()), 7);
	ASSERT(key.has_value());
	const auto first = i1->order[0];
	i1->separators[first] = *key;
	i1->separatorsCV[first] = 7;

	Access::updateSeparator(t.tree, i1);
	ASSERT_EQ(t.keys().compareKeys(asRef(k), separatorOf(&i1->header)), 0);
	ASSERT_EQ(separatorCVOf(&i1->header), 7);

	const RangeRef rootFirst = root->separators[root->order[0]];
	Access::updateSeparator(t.tree, root);
	ASSERT(root->separators[root->order[0]].firstBlock == rootFirst.firstBlock);

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
	// The writes reach two levels at any block size, and three at 2 KiB;
	// larger nodes need far more writes for a third.
	BNodeHeader* root = Access::root(t.tree);
	ASSERT(root->kind == BNodeKind::Internal);
	ASSERT(BlockSize > 2048 ||
		   childAt(reinterpret_cast<InternalBlock*>(root), 0)->kind == BNodeKind::Internal);
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

// A write that starts after the last entry of a leaf and ends inside the
// first range of the next leaf goes into the leaf findKey reached, after its
// last entry. The range of the next leaf keeps its right exterior, so the
// separators of that leaf and of its parent become the new start, with the
// range's CV.
TEST_CASE("/resolver/TempConflictBTree/AddWriteTrimsFirstRangeOfLeaf") {
	TestTree t;
	std::vector<Entry> model = spacedRanges(t, 6, 3, 3);
	LeafBlock* previous = t.leaves[2].leaf;
	LeafBlock* leaf = t.leaves[3].leaf;
	InternalBlock* parent = leaf->parent();
	ASSERT(childAt(parent, 0) == &leaf->header);
	const Version cv = t.leaves[3].entries.front().cv;
	checkWrite(t, model, wideKey(275), wideKey(285), 1000);
	ASSERT_EQ(previous->header.count, 4);
	const auto last = previous->order[3];
	ASSERT_EQ(t.keys().compareBegin(asRef(wideKey(275)), previous->ranges[last]), 0);
	ASSERT_EQ(previous->cv[last], 1000);
	ASSERT(leaf->parent() == parent);
	ASSERT_EQ(leaf->header.count, 3);
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(285)), separatorOf(&leaf->header)), 0);
	ASSERT_EQ(separatorCVOf(&leaf->header), cv);
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(285)), separatorOf(&parent->header)), 0);
	ASSERT_EQ(separatorCVOf(&parent->header), cv);
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
									leaf->ranges[leaf->order[0]]), 0);
	ASSERT_EQ(leaf->cv[leaf->order[0]], cv);
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(285)), separatorOf(&leaf->header)), 0);
	ASSERT_EQ(separatorCVOf(&leaf->header), cv);
	ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(285)), separatorOf(&parent->header)), 0);
	t.leaves[3].entries.front().begin = wideKey(285);
	checkSameEntries(checkTree(t), modelEntries(t));

	// Not the first entry: only the range changes.
	const RangeRef separator = separatorOf(&leaf->header);
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
	const RangeRef& range = leaf->ranges[leaf->order[rank]];
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
										leaf->ranges[leaf->order[rank]]), 0);
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
			const auto first = erasedFrom->order[0];
			ASSERT_EQ(t.keys().compareBegin(
						  [&] {
							  const RangeRef k = separatorOf(&erasedFrom->header);
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
	leaf->cv[leaf->order[0]] = 100;
	i0->separatorsCV[leaf->header.parentSlot] = 100;
	t.leaves[1].entries.front().cv = 100;
	// The entries with CVs 2..cut expire, at least two, so the leaf has room
	// again; the cut stays below the CV 100 of the other leaves.
	const Version cut = std::min<Version>(50, LeafCapacity / 2);
	Access::setReclaimCV(t.tree, cut);

	const Position p = find(t, t.leaves[1].lowerBound);
	ASSERT(p.leaf == leaf);
	ASSERT(p.nodeNeedsSplit == nullptr);
	ASSERT_EQ(leaf->header.count, LeafCapacity - (cut - 1));
	ASSERT_EQ(i0->header.childrenNeedingSplit, 0);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	std::vector<Entry>& entries = t.leaves[1].entries;
	entries.erase(std::remove_if(entries.begin(), entries.end(),
								 [&](const Entry& e) { return e.cv <= cut; }),
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

namespace {

// A key of 2023 bytes starting as key(value): its point range is stored as a
// record of 2024 bytes, which fills a key block on its own.
Bytes longKey(unsigned value) {
	Bytes k = key(value);
	k.resize(BlockSize - sizeof(KeyBlockHeader) - 1, 'x');
	return k;
}

Entry longPoint(unsigned value, Version cv) {
	return {longKey(value), keyAfter(longKey(value)), cv};
}

std::vector<Entry> liveEntries(const std::vector<Entry>& entries, Version cut) {
	std::vector<Entry> live;
	for (const Entry& e : entries) {
		if (e.cv > cut) {
			live.push_back(e);
		}
	}
	return live;
}

// Takes the blocks the pool has free, writing over them, so that stale
// references to reclaimed keys cannot keep working by accident.
void reuseFreeBlocks(TestTree& t) {
	PagePool& pool = Access::pool(t.tree);
	std::vector<std::byte*> blocks;
	for (std::byte* b = pool.allocateBlock(); b != nullptr; b = pool.allocateBlock()) {
		std::memset(b, 0xa5, pool.blockSize());
		blocks.push_back(b);
	}
	for (std::byte* b : blocks) {
		pool.releaseBlock(b);
	}
}

// root -> [L0, L1, L2]. L1 starts with a live entry, so its separator does
// not expire, followed by two entries with CVs firstDead and secondDead
// whose keys fill the first two key blocks. Every other key, and every
// node, is newer. L2's lower bound is below its first key.
std::vector<Entry> deadNeighbourTree(TestTree& t, Version firstDead, Version secondDead) {
	const Entry d1 = longPoint(22, firstDead);
	const Entry d2 = longPoint(24, secondDead);
	const std::optional<RangeRef> r1 =
		t.keys().appendRange(asRef(d1.begin), asRef(d1.end), d1.cv);
	const std::optional<RangeRef> r2 =
		t.keys().appendRange(asRef(d2.begin), asRef(d2.end), d2.cv);
	ASSERT(r1.has_value() && r2.has_value());
	ASSERT(r1->firstBlock != r2->firstBlock);
	t.addLeaf({}, entriesFrom(10, 3, 50));
	const Entry a = entriesFrom(20, 1, 60).front();
	const std::optional<RangeRef> ra =
		t.keys().appendRange(asRef(a.begin), asRef(a.end), a.cv);
	ASSERT(ra.has_value());
	t.addLeaf(key(20), {a, d1, d2}, {*ra, *r1, *r2});
	t.addLeaf(key(28), entriesFrom(30, 3, 70));
	t.build(3);
	return modelEntries(t);
}

// Reclaims the two key blocks of the dead entries and reuses them.
void reclaimDeadKeys(TestTree& t) {
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 6);
	ASSERT(!t.keys().isValid(t.leaves[1].leaf->ranges[t.leaves[1].leaf->order[1]]));
	reuseFreeBlocks(t);
}

} // namespace

// A read that starts in L0 and runs into L1 crosses its dead entries without
// reading their keys.
TEST_CASE("/resolver/TempConflictBTree/HasConflictSkipsReclaimedKeysToTheRight") {
	TestTree t;
	const std::vector<Entry> model = deadNeighbourTree(t, 5, 6);
	reclaimDeadKeys(t);
	ASSERT(!t.hasConflict(key(13), key(26), 100));
	ASSERT(t.hasConflict(key(13), key(26), 55));
	checkSameEntries(liveEntries(checkTree(t), 6), liveEntries(model, 6));
}

// A read that starts in L2, below its first key, finds a dead entry as the
// previous one and does not read its key.
TEST_CASE("/resolver/TempConflictBTree/HasConflictSkipsReclaimedPreviousKey") {
	TestTree t;
	const std::vector<Entry> model = deadNeighbourTree(t, 5, 6);
	reclaimDeadKeys(t);
	ASSERT(!t.hasConflict(key(29), key(30), 6));
	ASSERT(t.hasConflict(key(29), key(31), 6));
	checkSameEntries(liveEntries(checkTree(t), 6), liveEntries(model, 6));
}

// A write from L0 into L2 erases what it covers, dead entries included,
// without reading reclaimed keys.
TEST_CASE("/resolver/TempConflictBTree/AddWriteOverReclaimedKeys") {
	TestTree t;
	std::vector<Entry> model = liveEntries(deadNeighbourTree(t, 5, 6), 6);
	reclaimDeadKeys(t);
	checkWrite(t, model, key(13), key(31), 200);
	checkWrite(t, model, key(21), key(23), 201);
}

// A write that starts in L2, below its first key, has a dead entry of L1 as
// the previous one: it is erased unread, and the write goes on from there.
TEST_CASE("/resolver/TempConflictBTree/AddWriteAfterReclaimedPreviousKey") {
	TestTree t;
	std::vector<Entry> model = liveEntries(deadNeighbourTree(t, 5, 6), 6);
	reclaimDeadKeys(t);
	checkWrite(t, model, key(29), key(30), 200);
	ASSERT_EQ(leafEntries(t, t.leaves[1].leaf).size(), 1);
	checkWrite(t, model, key(25), key(29), 201);
}

// A root leaf whose entries have all expired is emptied by the search: a
// read finds neither a previous entry nor one at its position.
TEST_CASE("/resolver/TempConflictBTree/HasConflictInExpiredRootLeaf") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 2, 0), {5, 6}));
	t.build(2);
	Access::setReclaimCV(t.tree, 10);
	ASSERT(!t.hasConflict(key(11), key(20), 10));
	ASSERT_EQ(t.leaves[0].leaf->header.count, 0);
	ASSERT(Access::root(t.tree) == &t.leaves[0].leaf->header);
	checkTree(t);
}

// Key blocks are reclaimed in allocation order, and a later block can hold
// only older records. Reclaiming it must not move the cut back, or entries
// whose keys are gone would count as live again.
TEST_CASE("/resolver/TempConflictBTree/ReclaimCVNeverDecreases") {
	TestTree t;
	const std::vector<Entry> model = deadNeighbourTree(t, 6, 5);
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 6);
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 6);
	reuseFreeBlocks(t);
	ASSERT(!t.hasConflict(key(21), key(25), 6));
	checkSameEntries(liveEntries(checkTree(t), 6), liveEntries(model, 6));
}


namespace {

std::vector<BNodeHeader*> ageListOf(TestTree& t) {
	std::vector<BNodeHeader*> nodes;
	for (BNodeHeader* n = t.nodes().oldestNode(); n != nullptr; n = n->ageNext) {
		nodes.push_back(n);
	}
	return nodes;
}

void collectNodes(BNodeHeader* node, std::vector<BNodeHeader*>& nodes) {
	nodes.push_back(node);
	if (node->kind == BNodeKind::Internal) {
		auto* internal = reinterpret_cast<InternalBlock*>(node);
		for (std::size_t r = 0; r < internal->header.count; ++r) {
			collectNodes(childAt(internal, r), nodes);
		}
	}
}

std::vector<BNodeHeader*> treeNodes(TestTree& t) {
	std::vector<BNodeHeader*> nodes;
	if (Access::root(t.tree) != nullptr) {
		collectNodes(Access::root(t.tree), nodes);
	}
	return nodes;
}

// The age list holds exactly the nodes in the tree.
void checkAgeList(TestTree& t) {
	std::vector<BNodeHeader*> listed = ageListOf(t);
	std::vector<BNodeHeader*> reachable = treeNodes(t);
	std::sort(listed.begin(), listed.end());
	std::sort(reachable.begin(), reachable.end());
	ASSERT(listed == reachable);
}

bool orderIsIdentity(const InternalBlock* node) {
	for (std::size_t r = 0; r < node->header.count; ++r) {
		if (node->order[r] != r) {
			return false;
		}
	}
	return true;
}

// Removes the subtree under node and checks that its nodes, and any
// ancestor left empty or replaced as root, are gone from the age list and
// back in the pool, and that the rest of the tree holds every other entry.
void removeAndCheck(TestTree& t, BNodeHeader* node) {
	std::vector<BNodeHeader*> removed;
	collectNodes(node, removed);
	std::vector<LeafBlock*> leaves;
	collectLeaves(node, leaves);
	const std::size_t nodesBefore = ageListOf(t).size();
	const std::size_t available = Access::pool(t.tree).availableBlocks();
	Access::removeSubtree(t.tree, node);
	for (LeafModel& l : t.leaves) {
		if (std::find(leaves.begin(), leaves.end(), l.leaf) != leaves.end()) {
			l.entries.clear();
		}
	}
	const std::vector<BNodeHeader*> listed = ageListOf(t);
	for (BNodeHeader* r : removed) {
		ASSERT(std::find(listed.begin(), listed.end(), r) == listed.end());
	}
	ASSERT(listed.size() <= nodesBefore - removed.size());
	ASSERT_EQ(Access::pool(t.tree).availableBlocks(),
			  available + nodesBefore - listed.size());
	checkAgeList(t);
	checkSameEntries(checkTree(t), modelEntries(t));
}

} // namespace

TEST_CASE("/resolver/TempConflictBTree/RemoveSubtreeOfRootLeaf") {
	TestTree t;
	t.addLeaf({}, entriesFrom(10, 2, 1));
	t.build(2);
	removeAndCheck(t, Access::root(t.tree));
	ASSERT(Access::root(t.tree) == nullptr);
	ASSERT(t.nodes().oldestNode() == nullptr);
}

TEST_CASE("/resolver/TempConflictBTree/RemoveSubtreeOfWholeTree") {
	TestTree t;
	spacedRanges(t, 9, 2, 3);
	ASSERT_EQ(treeNodes(t).size(), 13);
	removeAndCheck(t, Access::root(t.tree));
	ASSERT(Access::root(t.tree) == nullptr);
	ASSERT(t.nodes().oldestNode() == nullptr);
}

// Leaves and internal nodes at the first, middle and last ranks of parents
// whose physical slots are not their ranks.
TEST_CASE("/resolver/TempConflictBTree/RemoveSubtreeAtEachPosition") {
	for (bool internal : {false, true}) {
		for (std::size_t rank = 0; rank < 3; ++rank) {
			TestTree t;
			spacedRanges(t, 9, 1, 3);
			InternalBlock* root = rootNode(t);
			InternalBlock* parent =
				internal ? root : reinterpret_cast<InternalBlock*>(childAt(root, 1));
			ASSERT(!orderIsIdentity(parent));
			removeAndCheck(t, childAt(parent, rank));
			ASSERT_EQ(parent->header.count, 2);
		}
	}
}

// Removing the last child of a node removes the node too, and the root left
// with one child is replaced by it.
TEST_CASE("/resolver/TempConflictBTree/RemoveSubtreeCollapsesRoot") {
	// root -> [I0 -> [L0, L1], I1 -> [L2]].
	TestTree t;
	spacedRanges(t, 3, 1, 2);
	InternalBlock* i0 = t.leaves[0].leaf->parent();
	const std::size_t nodes = ageListOf(t).size();
	removeAndCheck(t, &t.leaves[2].leaf->header);
	// L2, I1 left empty, and the root replaced by I0.
	ASSERT_EQ(ageListOf(t).size(), nodes - 3);
	ASSERT(Access::root(t.tree) == &i0->header);
	ASSERT(t.leaves[1].leaf->right == nullptr);
}

// A removed child that needed a split is no longer counted above.
TEST_CASE("/resolver/TempConflictBTree/RemoveSubtreeUpdatesSplitCounts") {
	TestTree t;
	InternalBlock* i0 = fullLeafUnderCriticalNode(t);
	InternalBlock* root = rootNode(t);
	removeAndCheck(t, &t.leaves[1].leaf->header);
	ASSERT_EQ(i0->header.childrenNeedingSplit, 0);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
}

TEST_CASE("/resolver/TempConflictBTree/ReclaimOldestWithNothingToReclaim") {
	TestTree t;
	const std::size_t available = Access::pool(t.tree).availableBlocks();
	ASSERT(!Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 0);
	ASSERT_EQ(Access::pool(t.tree).availableBlocks(), available);
}

// Only key blocks to reclaim. The record fills its block exactly, so the
// store opens a new, empty block, with CV 0: an older age than the cut,
// which reclaiming leaves where it is.
TEST_CASE("/resolver/TempConflictBTree/ReclaimOldestOnlyKeys") {
	TestTree t;
	const std::size_t initial = Access::pool(t.tree).availableBlocks();
	const Entry e = longPoint(10, 7);
	ASSERT(t.keys().appendRange(asRef(e.begin), asRef(e.end), e.cv).has_value());
	ASSERT_EQ(Access::pool(t.tree).availableBlocks(), initial - 2);
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 7);
	ASSERT_EQ(*t.keys().oldestCV(), 0);
	ASSERT_EQ(Access::pool(t.tree).availableBlocks(), initial - 1);
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 7);
	ASSERT(!t.keys().oldestCV().has_value());
	ASSERT_EQ(Access::pool(t.tree).availableBlocks(), initial);
	ASSERT(!Access::reclaimOldest(t.tree));
}

// Only nodes to reclaim: an empty root leaf, with CV 0, which is a CV like
// any other.
TEST_CASE("/resolver/TempConflictBTree/ReclaimOldestOnlyNodes") {
	TestTree t;
	t.addLeaf({}, {});
	t.build(2);
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 0);
	ASSERT(Access::root(t.tree) == nullptr);
	ASSERT(t.nodes().oldestNode() == nullptr);
	ASSERT(!Access::reclaimOldest(t.tree));
}

// The oldest key block is older than every node: only it goes.
TEST_CASE("/resolver/TempConflictBTree/ReclaimOldestKeysFirst") {
	TestTree t;
	const std::vector<Entry> model = deadNeighbourTree(t, 5, 6);
	const std::vector<BNodeHeader*> nodes = ageListOf(t);
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 5);
	ASSERT(ageListOf(t) == nodes);
	ASSERT_EQ(*t.keys().oldestCV(), 6);
	reuseFreeBlocks(t);
	checkSameEntries(liveEntries(checkTree(t), 5), liveEntries(model, 5));
}

namespace {

// Leaves with entries of the given CVs, short keys all in one key block,
// under a single root. A leaf's CV is the largest of its entries and of the
// leaves before it.
std::vector<Entry> agedLeaves(TestTree& t, const std::vector<Version>& cvs) {
	for (std::size_t i = 0; i < cvs.size(); ++i) {
		const unsigned first = 10 + 10 * static_cast<unsigned>(i);
		t.addLeaf(i == 0 ? Bytes{} : key(first),
				  withCVs(entriesFrom(first, 2, 0), {cvs[i], cvs[i]}));
	}
	t.build(cvs.size());
	return modelEntries(t);
}

} // namespace

// The oldest node is older than the key block: only it goes.
TEST_CASE("/resolver/TempConflictBTree/ReclaimOldestNodesFirst") {
	TestTree t;
	const std::vector<Entry> model = agedLeaves(t, {10, 20, 30});
	const Version keyCV = *t.keys().oldestCV();
	ASSERT_EQ(keyCV, 30);
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 10);
	ASSERT_EQ(*t.keys().oldestCV(), keyCV);
	ASSERT_EQ(rootNode(t)->header.count, 2);
	ASSERT(t.leaves[1].leaf->left == nullptr);
	checkAgeList(t);
	checkSameEntries(liveEntries(checkTree(t), 10), liveEntries(model, 10));
}

// A tie between the stores, and several nodes of the same age: one call
// reclaims all of them, and the root left with one child is replaced.
TEST_CASE("/resolver/TempConflictBTree/ReclaimOldestTiesAndEqualAges") {
	TestTree t;
	const Entry k = longPoint(200, 10);
	ASSERT(t.keys().appendRange(asRef(k.begin), asRef(k.end), k.cv).has_value());
	const std::vector<Entry> model = agedLeaves(t, {10, 10, 30});
	ASSERT_EQ(*t.keys().oldestCV(), 10);
	ASSERT_EQ(*t.nodes().oldestCV(), 10);
	const std::size_t available = Access::pool(t.tree).availableBlocks();
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 10);
	ASSERT_EQ(*t.keys().oldestCV(), 30);
	// Two leaves, the old root and one key block.
	ASSERT_EQ(Access::pool(t.tree).availableBlocks(), available + 4);
	ASSERT(Access::root(t.tree) == &t.leaves[2].leaf->header);
	checkAgeList(t);
	checkSameEntries(liveEntries(checkTree(t), 10), liveEntries(model, 10));
}

// Each call reclaims one age only; later ages stay for later calls, and the
// cut only moves forward.
TEST_CASE("/resolver/TempConflictBTree/ReclaimOldestOneAgeAtATime") {
	TestTree t;
	const std::vector<Entry> model = agedLeaves(t, {10, 20, 30});
	for (Version cut : {10, 20}) {
		ASSERT(Access::reclaimOldest(t.tree));
		ASSERT_EQ(t.tree.reclaimCV(), cut);
		ASSERT(*t.nodes().oldestCV() > cut);
		checkAgeList(t);
		checkSameEntries(liveEntries(checkTree(t), cut), liveEntries(model, cut));
	}
	// The last leaf, the root and the key block share the last age.
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 30);
	ASSERT(Access::root(t.tree) == nullptr);
	ASSERT(!t.keys().oldestCV().has_value());
	ASSERT(!Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 30);
}

// A node's CV is a conservative bound: a leaf whose entries are older keeps
// them, expired, until the cut reaches the leaf's CV.
TEST_CASE("/resolver/TempConflictBTree/ReclaimOldestWaitsForConservativeCV") {
	TestTree t;
	const Entry k = longPoint(200, 15);
	ASSERT(t.keys().appendRange(asRef(k.begin), asRef(k.end), k.cv).has_value());
	const std::vector<Entry> model = agedLeaves(t, {20, 15, 30});
	LeafBlock* leaf = t.leaves[1].leaf;
	ASSERT_EQ(leaf->header.maxCV, 20);
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 15);
	ASSERT(leaf->parent() != nullptr);
	ASSERT_EQ(leaf->header.count, 2);
	checkSameEntries(liveEntries(checkTree(t), 15), liveEntries(model, 15));
	ASSERT(Access::reclaimOldest(t.tree));
	ASSERT_EQ(t.tree.reclaimCV(), 20);
	checkAgeList(t);
	ASSERT(Access::root(t.tree) == &t.leaves[2].leaf->header);
	checkSameEntries(liveEntries(checkTree(t), 20), liveEntries(model, 20));
}

TEST_CASE("/resolver/TempConflictBTree/ReclaimUntilAvailCases") {
	// Enough already, or n == 0: nothing is reclaimed.
	for (bool zero : {false, true}) {
		TestTree t;
		agedLeaves(t, {10, 20, 30});
		const std::size_t available = Access::pool(t.tree).availableBlocks();
		ASSERT(Access::reclaimUntilAvail(t.tree, zero ? 0 : available));
		ASSERT_EQ(t.tree.reclaimCV(), 0);
		ASSERT_EQ(Access::pool(t.tree).availableBlocks(), available);
	}
	// One age is enough, then two are needed.
	for (std::size_t more : {std::size_t{1}, std::size_t{2}}) {
		TestTree t;
		const std::vector<Entry> model = agedLeaves(t, {10, 20, 30});
		const std::size_t available = Access::pool(t.tree).availableBlocks();
		ASSERT(Access::reclaimUntilAvail(t.tree, available + more));
		ASSERT_EQ(t.tree.reclaimCV(), more == 1 ? 10 : 20);
		// L0; then L1 too, and the root, replaced by L2.
		ASSERT_EQ(Access::pool(t.tree).availableBlocks(),
				  available + (more == 1 ? 1 : 3));
		checkAgeList(t);
		checkSameEntries(liveEntries(checkTree(t), t.tree.reclaimCV()),
						 liveEntries(model, t.tree.reclaimCV()));
	}
	// A single age frees more than asked for: the whole tree.
	{
		TestTree t;
		agedLeaves(t, {10, 10, 10});
		const std::size_t available = Access::pool(t.tree).availableBlocks();
		ASSERT(Access::reclaimUntilAvail(t.tree, available + 1));
		ASSERT_EQ(Access::pool(t.tree).availableBlocks(), available + 5);
		ASSERT(Access::root(t.tree) == nullptr);
	}
	// More than everything: reclaims all and reports failure.
	{
		TestTree t;
		agedLeaves(t, {10, 20, 30});
		const std::size_t available = Access::pool(t.tree).availableBlocks();
		ASSERT(!Access::reclaimUntilAvail(t.tree, available + 100));
		ASSERT_EQ(Access::pool(t.tree).availableBlocks(), available + 5);
		ASSERT(Access::root(t.tree) == nullptr);
		ASSERT(!t.keys().oldestCV().has_value());
	}
}

// Writes with long keys in a small pool, so that every write may reclaim,
// and explicit reclamations between them. After each operation the
// structure holds the live entries of the model; every 50 operations, and
// at the end, the conflicts match too. Freed blocks are reused so that
// stale references fail.
TEST_CASE("/resolver/TempConflictBTree/ReclaimAndWriteAgainstModel") {
	TestTree t(4 * PageSize);
	std::vector<Entry> model;
	std::uint32_t state = 99;
	auto next = [&state]() {
		state = state * 1103515245u + 12345u;
		return state >> 8;
	};
	auto keyOf = [](unsigned v) {
		Bytes k = wideKey(v);
		k.resize(3 + v % 300, static_cast<uint8_t>('a' + v % 26));
		return k;
	};
	unsigned reclaims = 0;
	for (unsigned n = 0; n < 1500; ++n) {
		const Version cv = 1000 + n;
		if (n % 7 == 6) {
			reclaims += Access::reclaimOldest(t.tree);
		} else {
			const unsigned begin = next() % 2000;
			const unsigned length = 1 + next() % (n % 10 == 0 ? 200 : 6);
			const Bytes b = keyOf(begin);
			const Bytes e = keyOf(begin + length);
			model = applyWrite(model, b, e, cv);
			Access::addWrite(t.tree, asRef(b), asRef(e), cv);
		}
		const Version cut = t.tree.reclaimCV();
		model = liveEntries(model, cut);
		if (n % 50 == 49) {
			reuseFreeBlocks(t);
			checkSameEntries(liveEntries(checkTree(t), cut), model);
		} else {
			checkSameEntries(liveEntries(checkStructure(t), cut), model);
		}
		checkAgeList(t);
	}
	ASSERT(reclaims > 0);
	ASSERT(t.tree.reclaimCV() > 1000);
	checkSameEntries(liveEntries(checkTree(t), t.tree.reclaimCV()), model);
}

namespace {

Entry wideRange(unsigned begin, unsigned end, Version cv) {
	return {wideKey(begin), wideKey(end), cv};
}

} // namespace

// A key absent from a leaf, past its last entry, has its position at the end
// of that leaf. The write goes there, never into the next leaf, even when that
// leaf is full: the leaf findKey reached grows and splits instead.
TEST_CASE("/resolver/TempConflictBTree/AddWriteAtEndOfLeafBeforeFullLeaf") {
	TestTree t;
	t.addLeaf({}, wideEntriesFrom(10, 3, 1));
	t.addLeaf(wideKey(1000), wideEntriesFrom(1000, LeafCapacity, 1));
	t.addLeaf(wideKey(2000), wideEntriesFrom(2000, 3, 1));
	t.build(3);
	std::vector<Entry> model = modelEntries(t);
	LeafBlock* full = t.leaves[1].leaf;
	ASSERT(!full->hasSpace());
	Version cv = 1000;
	for (unsigned k = 17; k < 17 + 2 * LeafCapacity; k += 2) {
		checkWrite(t, model, wideKey(k), keyAfter(wideKey(k)), cv++);
		ASSERT_EQ(full->header.count, LeafCapacity);
	}
	// The first leaf has split along the way.
	ASSERT(rootNode(t)->header.count > 3);
}

// A key found at the start of a range: the write goes before it, and the
// range keeps its right exterior with its CV, or goes if the write covers it.
// Also at the first entry of a leaf, whose separator follows.
TEST_CASE("/resolver/TempConflictBTree/AddWriteAtFoundKey") {
	struct Case {
		unsigned begin;
		unsigned end;
	};
	for (const Case c : {Case{120, 125}, Case{140, 155}, Case{280, 283}, Case{280, 295}}) {
		TestTree t;
		std::vector<Entry> model = spacedRanges(t, 6, 3, 3);
		checkWrite(t, model, wideKey(c.begin), wideKey(c.end), 1000);
	}
}

// The previous range is in the leaf before and reaches past the separator of
// the leaf findKey reached. It is trimmed in place; what the write inserts,
// the write and, when the write lies inside the range, its right exterior,
// goes first in the leaf findKey reached.
TEST_CASE("/resolver/TempConflictBTree/AddWriteInsideRangeOfPreviousLeaf") {
	for (unsigned end : {140u, 155u}) {
		TestTree t;
		t.addLeaf({}, {wideRange(100, 110, 5), wideRange(120, 150, 6)});
		t.addLeaf(wideKey(130), {wideRange(160, 170, 7), wideRange(180, 190, 8)});
		t.addLeaf(wideKey(200), {wideRange(200, 210, 9)});
		t.build(3);
		std::vector<Entry> model = modelEntries(t);
		LeafBlock* previous = t.leaves[0].leaf;
		LeafBlock* leaf = t.leaves[1].leaf;
		checkWrite(t, model, wideKey(135), wideKey(end), 1000);
		ASSERT_EQ(previous->header.count, 2);
		ASSERT_EQ(leaf->header.count, end == 140 ? 4 : 3);
		ASSERT_EQ(t.keys().compareKeys(asRef(wideKey(135)), separatorOf(&leaf->header)), 0);
	}
}

// A write from the end of a leaf across a whole internal node and into the
// first range after it: the leaves and the node go, and the write stays in
// the leaf it started from.
TEST_CASE("/resolver/TempConflictBTree/AddWriteRemovesSubtreeFromEndOfLeaf") {
	TestTree t;
	std::vector<Entry> model = spacedRanges(t, 9, 3, 3);
	InternalBlock* root = rootNode(t);
	LeafBlock* leaf = t.leaves[2].leaf;
	ASSERT_EQ(root->header.count, 3);
	checkWrite(t, model, wideKey(275), wideKey(470), 1000);
	ASSERT_EQ(root->header.count, 2);
	ASSERT_EQ(leaf->header.count, 4);
	checkAgeList(t);
}

// A leaf with exactly two free slots does not need a split, and a write
// inside one of its ranges takes both: the write and its right exterior.
// The next such write finds the leaf full and splits it first.
TEST_CASE("/resolver/TempConflictBTree/AddWriteInsideRangeTakesTwoSlots") {
	TestTree t;
	std::vector<Entry> entries;
	for (unsigned i = 0; i < LeafCapacity - 2; ++i) {
		entries.push_back(wideRange(100 + 20 * i, 110 + 20 * i, 1));
	}
	t.addLeaf({}, entries);
	t.build(2);
	std::vector<Entry> model = modelEntries(t);
	LeafBlock* leaf = t.leaves[0].leaf;
	ASSERT_EQ(leaf->freeSlotCount(), 2);
	checkWrite(t, model, wideKey(103), wideKey(105), 1000);
	ASSERT(!leaf->hasSpace());
	checkWrite(t, model, wideKey(123), wideKey(125), 1001);
	ASSERT(Access::root(t.tree)->kind == BNodeKind::Internal);
}

// The batch pattern that overflowed a leaf in the smoke test: scattered
// point reads and writes over 100,000 keys, with many keys falling past the
// last entry of a leaf. Every read is checked against a model of the points,
// and the entries against the model at the end.
TEST_CASE("/resolver/TempConflictBTree/ScatteredPointsRegression") {
	TestTree t(256 * PageSize);
	std::uint64_t state = 1001 * 0x9E3779B97F4A7C15ull + 1;
	auto below = [&state](int n) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return static_cast<int>(state % static_cast<std::uint64_t>(n));
	};
	auto pointKey = [](int n) {
		char k[16];
		std::snprintf(k, sizeof k, "k%08d", n);
		return Bytes(k, k + 9);
	};
	std::map<Bytes, Version> written;
	Version now = 2000;
	for (int b = 0; b < 2500; ++b, now += 10) {
		std::vector<int> writes;
		for (int i = 0; i < 20; ++i) {
			const Version snapshot = now - 1 - below(1200);
			for (int r = below(4); r > 0; --r) {
				const Bytes k = pointKey(below(100000));
				if (snapshot >= now - 1000) {
					auto it = written.find(k);
					const bool expected = it != written.end() && it->second > snapshot;
					ASSERT_EQ(t.hasConflict(k, keyAfter(k), snapshot), expected);
				}
			}
			for (int w = below(3); w > 0; --w) {
				writes.push_back(below(100000));
			}
		}
		std::sort(writes.begin(), writes.end());
		writes.erase(std::unique(writes.begin(), writes.end()), writes.end());
		for (int n : writes) {
			const Bytes k = pointKey(n);
			Access::addWrite(t.tree, asRef(k), asRef(keyAfter(k)), now);
			written[k] = now;
		}
		if (b % 500 == 499) {
			checkStructure(t);
		}
	}
	std::vector<Entry> model;
	for (const auto& [k, cv] : written) {
		model.push_back({k, keyAfter(k), cv});
	}
	checkSameEntries(checkStructure(t), model);
}

namespace {

// One transaction of a read batch: its read ranges, its read version,
// whether it reports every conflicting range, and whether an earlier check
// of the batch already marked it conflicting.
struct BatchTransaction {
	std::vector<std::pair<Bytes, Bytes>> reads;
	Version version = 0;
	bool report = false;
	bool alreadyConflicting = false;
};

struct BatchResult {
	std::vector<uint8_t> status;     // per transaction
	std::vector<uint8_t> conflicts;  // per read, in transaction order
	std::vector<bool> tooOld;        // per transaction
	int64_t tooOldWithinWindow = 0;
};

// Calls checkReads<G> on a batch laid out as FDB's ConflictBatch lays it
// out: the reads of each transaction consecutive, and the begin and end of
// every read range, plus those of optional write ranges, in points sorted
// by key.
template<uint8_t G>
BatchResult checkBatch(TestTree& t, const std::vector<BatchTransaction>& batch,
					   const std::vector<std::pair<Bytes, Bytes>>& writes = {}) {
	const std::size_t T = batch.size();
	std::vector<std::vector<std::pair<int, int>>> readSlots(T);
	std::vector<std::pair<int, int>> writeSlots(writes.size());
	std::vector<TransactionInfo> infos(T);
	std::vector<TransactionInfo*> infoPointers(T);
	std::vector<VectorRef<int>> reported(T);
	std::vector<ReadConflictRange> reads;
	std::vector<int> offsets(T);
	for (std::size_t tx = 0; tx < T; ++tx) {
		offsets[tx] = static_cast<int>(reads.size());
		readSlots[tx].resize(batch[tx].reads.size());
		for (std::size_t r = 0; r < batch[tx].reads.size(); ++r) {
			reads.emplace_back(asRef(batch[tx].reads[r].first),
							   asRef(batch[tx].reads[r].second),
							   batch[tx].version, static_cast<int>(tx),
							   static_cast<int>(r),
							   batch[tx].report ? &reported[tx] : nullptr);
		}
		infos[tx].readRanges = VectorRef<std::pair<int, int>>(
			readSlots[tx].data(), static_cast<int>(readSlots[tx].size()));
		infos[tx].tooOld = false;
		infos[tx].reportConflictingKeys = batch[tx].report;
		infoPointers[tx] = &infos[tx];
	}
	// The write ranges belong to the first transaction.
	if (!writes.empty()) {
		ASSERT(T > 0);
		infos[0].writeRanges = VectorRef<std::pair<int, int>>(
			writeSlots.data(), static_cast<int>(writeSlots.size()));
	}

	std::vector<KeyInfo> points;
	for (std::size_t tx = 0; tx < T; ++tx) {
		for (std::size_t r = 0; r < batch[tx].reads.size(); ++r) {
			const ReadConflictRange& read = reads[offsets[tx] + r];
			points.emplace_back(read.begin, true, false, static_cast<int>(tx),
								&readSlots[tx][r].first);
			points.emplace_back(read.end, false, false, static_cast<int>(tx),
								&readSlots[tx][r].second);
		}
	}
	for (std::size_t w = 0; w < writes.size(); ++w) {
		points.emplace_back(asRef(writes[w].first), true, true, 0, &writeSlots[w].first);
		points.emplace_back(asRef(writes[w].second), false, true, 0, &writeSlots[w].second);
	}
	std::stable_sort(points.begin(), points.end(),
					 [](const KeyInfo& a, const KeyInfo& b) {
						 return a.key.compare(b.key) < 0;
					 });

	BatchResult result;
	result.status.resize(T);
	for (std::size_t tx = 0; tx < T; ++tx) {
		result.status[tx] = batch[tx].alreadyConflicting;
	}
	t.tree.checkReads<G>(points, infoPointers.data(), reads, offsets,
						 reinterpret_cast<bool*>(result.status.data()),
						 result.conflicts, result.tooOldWithinWindow);
	for (const TransactionInfo& info : infos) {
		result.tooOld.push_back(info.tooOld);
	}
	return result;
}

// Checks a batch's result against a brute-force scan of the tree's entries:
// a transaction is marked when it was already, or when one of its non-empty
// reads conflicts; a reporting transaction has each conflicting read marked;
// no read is marked without a conflict. The batch must hold no too-old read.
void checkBatchAgainstReference(const TestTree& t,
								const std::vector<BatchTransaction>& batch,
								const BatchResult& result) {
	std::size_t i = 0;
	for (std::size_t tx = 0; tx < batch.size(); ++tx) {
		ASSERT(!result.tooOld[tx]);
		bool any = false;
		for (const auto& [begin, end] : batch[tx].reads) {
			const bool expected =
				begin < end && t.expectedConflict(begin, end, batch[tx].version);
			any |= expected;
			if (result.conflicts[i]) {
				ASSERT(expected);
			}
			if (batch[tx].report && !batch[tx].alreadyConflicting) {
				ASSERT_EQ(bool(result.conflicts[i]), expected);
			}
			++i;
		}
		ASSERT_EQ(bool(result.status[tx]), batch[tx].alreadyConflicting || any);
	}
	ASSERT_EQ(result.conflicts.size(), i);
	ASSERT_EQ(result.tooOldWithinWindow, 0);
}

// Random reads over keys from k(0) to k(lastValue): points, short ranges,
// long ranges and empty ones, some repeated, in transactions of one to four
// reads at versions below maxVersion, a third of them reporting.
template<typename Next>
std::vector<BatchTransaction> randomBatch(Next& next, std::size_t transactions,
										  unsigned lastValue, Version maxVersion,
										  Bytes (*makeKey)(unsigned)) {
	std::vector<BatchTransaction> batch(transactions);
	std::vector<std::pair<Bytes, Bytes>> seen;
	for (BatchTransaction& tx : batch) {
		tx.version = static_cast<Version>(next() % maxVersion);
		tx.report = next() % 3 == 0;
		for (unsigned n = 1 + next() % 4; n > 0; --n) {
			const unsigned b = next() % (lastValue + 1);
			const unsigned kind = next() % 10;
			if (kind == 0 && !seen.empty()) {
				tx.reads.push_back(seen[next() % seen.size()]);
			} else if (kind < 4) {
				tx.reads.emplace_back(makeKey(b), keyAfter(makeKey(b)));
			} else if (kind < 7) {
				tx.reads.emplace_back(makeKey(b), makeKey(b + 1 + next() % 6));
			} else if (kind < 9) {
				tx.reads.emplace_back(makeKey(b), makeKey(b + 1 + next() % (lastValue + 1)));
			} else {
				tx.reads.emplace_back(makeKey(b + 1), makeKey(b));
			}
			seen.push_back(tx.reads.back());
		}
	}
	return batch;
}

} // namespace

TEST_CASE("/resolver/TempConflictBTree/CheckReadsInEmptyTree") {
	TestTree t;
	const std::vector<BatchTransaction> batch = {
		{{{key(1), key(9)}}, 0},
		{{{key(3), keyAfter(key(3))}, {key(5), key(6)}}, 4, true},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0, 0}));
}

// A batch with no read, only write ranges, leaves every status unchanged.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsWithoutReads") {
	TestTree t;
	t.addLeaf({}, entriesFrom(10, 5, 20));
	t.build(3);
	const std::vector<BatchTransaction> batch = {{{}, 0}, {{}, 0, false, true}};
	const BatchResult result =
		checkBatch<16>(t, batch, {{key(10), key(30)}, {key(40), key(50)}});
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0, 1}));
	ASSERT(result.conflicts.empty());
}

// Reads around a single stored range [k(20), k(30)) with CV 10, in a root
// leaf: overlap and a newer CV conflict; ranges are half-open.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsAroundOneRange") {
	TestTree t;
	t.addLeaf({}, {{key(20), key(30), 10}});
	t.build(3);
	const std::vector<BatchTransaction> batch = {
		{{{key(20), key(21)}}, 9},
		{{{key(15), key(21)}}, 9},
		{{{key(25), key(26)}}, 9},
		{{{key(29), key(40)}}, 9},
		{{{key(25), key(26)}}, 10},
		{{{key(25), key(26)}}, 11},
		{{{key(10), key(20)}}, 0},
		{{{key(30), key(40)}}, 0},
		{{{key(10), key(15)}}, 0},
		{{{key(35), key(40)}}, 0},
	};
	for (bool report : {false, true}) {
		std::vector<BatchTransaction> b = batch;
		for (BatchTransaction& tx : b) {
			tx.report = report;
		}
		const BatchResult result = checkBatch<16>(t, b);
		checkBatchAgainstReference(t, b, result);
		ASSERT_EQ(result.status,
				  (std::vector<uint8_t>{1, 1, 1, 1, 0, 0, 0, 0, 0, 0}));
	}
}

// A stored point [k, k\0) conflicts only with reads containing k.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsWithPoint") {
	TestTree t;
	t.addLeaf({}, {{key(20), keyAfter(key(20)), 10}});
	t.build(3);
	const std::vector<BatchTransaction> batch = {
		{{{key(20), keyAfter(key(20))}}, 0},
		{{{key(10), key(21)}}, 0},
		{{{keyAfter(key(20)), key(30)}}, 0},
		{{{key(10), key(20)}}, 0},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{1, 1, 0, 0}));
}

// Empty reads, with begin at or after end, check nothing, even where a
// newer range is stored.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsSkipsEmptyReads") {
	TestTree t;
	t.addLeaf({}, {{key(20), key(30), 10}});
	t.build(3);
	const std::vector<BatchTransaction> batch = {
		{{{key(25), key(25)}}, 0, true},
		{{{key(28), key(22)}}, 0, true},
		{{{key(25), key(25)}, {key(26), key(27)}}, 0, true},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0, 0, 1}));
	ASSERT_EQ(result.conflicts, (std::vector<uint8_t>{0, 0, 0, 1}));
}

// A transaction already marked by an earlier check of the batch is not
// checked again unless it reports its conflicting ranges, and its flag is
// never cleared.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsSkipsConflictingTransactions") {
	TestTree t;
	t.addLeaf({}, {{key(20), key(30), 10}, {key(40), key(50), 10}});
	t.build(3);
	const std::vector<BatchTransaction> batch = {
		{{{key(21), key(22)}, {key(41), key(42)}}, 0, false, true},
		{{{key(21), key(22)}, {key(41), key(42)}, {key(32), key(38)}}, 0, true, true},
		{{{key(32), key(38)}}, 0, false, true},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{1, 1, 1}));
	ASSERT_EQ(result.conflicts, (std::vector<uint8_t>{0, 0, 1, 1, 0, 0}));
}

// A transaction that reports its conflicting ranges has each of them
// marked; any other is marked as conflicting, with at least one of them.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsReportsEveryConflictingRead") {
	TestTree t;
	t.addLeaf({}, entriesFrom(10, 8, 20));
	t.addLeaf(key(40), entriesFrom(40, 8, 30));
	t.build(2);
	const std::vector<std::pair<Bytes, Bytes>> reads = {
		{key(10), key(12)}, {key(13), key(14)}, {key(42), key(43)},
		{key(54), keyAfter(key(54))}, {key(60), key(70)},
	};
	const std::vector<BatchTransaction> batch = {
		{reads, 0, true},
		{reads, 0, false},
		{reads, 25, true},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(std::vector<uint8_t>(result.conflicts.begin(), result.conflicts.begin() + 5),
			  (std::vector<uint8_t>{1, 0, 1, 1, 0}));
	ASSERT(std::count(result.conflicts.begin() + 5, result.conflicts.begin() + 10, 1) >= 1);
	ASSERT_EQ(std::vector<uint8_t>(result.conflicts.begin() + 10, result.conflicts.end()),
			  (std::vector<uint8_t>{0, 0, 1, 1, 0}));
}

// The same read in several transactions, and twice in one, with the same
// outcome everywhere.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsDuplicateReads") {
	TestTree t;
	t.addLeaf({}, entriesFrom(10, 6, 20));
	t.addLeaf(key(30), entriesFrom(30, 6, 20));
	t.build(2);
	const std::pair<Bytes, Bytes> hit{key(31), key(33)};
	const std::pair<Bytes, Bytes> miss{key(23), key(29)};
	std::vector<BatchTransaction> batch;
	for (int i = 0; i < 6; ++i) {
		batch.push_back({{hit, miss, hit}, 0, i % 2 == 0});
		batch.push_back({{miss, miss}, 0, i % 2 == 1});
	}
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	for (std::size_t tx = 0; tx < batch.size(); ++tx) {
		ASSERT_EQ(result.status[tx], tx % 2 == 0 ? 1 : 0);
	}
}

// A long read spanning every leaf conflicts only through the last range of
// the last leaf; with an older version nothing conflicts.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsLongReadAcrossLeaves") {
	TestTree t;
	for (unsigned i = 0; i < 8; ++i) {
		std::vector<Entry> entries = entriesFrom(10 + 20 * i, 5, 5);
		for (Entry& e : entries) {
			e.cv = 5;
		}
		if (i == 7) {
			entries.back().cv = 50;
		}
		t.addLeaf(i == 0 ? Bytes{} : key(10 + 20 * i), std::move(entries));
	}
	t.build(3);
	const std::vector<BatchTransaction> batch = {
		{{{key(0), key(250)}}, 10, true},
		{{{key(0), key(158)}}, 10, true},
		{{{key(11), key(200)}}, 49, true},
		{{{key(11), key(200)}}, 50, true},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{1, 0, 1, 0}));
}

// A transaction whose read version is below reclaimCV is too old: it is
// marked, counted once, and none of its reads is checked.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsMarksTooOldTransactions") {
	TestTree t;
	t.addLeaf({}, {{key(20), key(30), 40}});
	t.build(3);
	Access::setReclaimCV(t.tree, 20);
	const std::vector<BatchTransaction> batch = {
		{{{key(21), key(22)}, {key(23), key(24)}}, 19, true},
		{{{key(21), key(22)}}, 20},
		{{{key(1), key(2)}}, 5},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	ASSERT_EQ(result.tooOld, (std::vector<bool>{true, false, true}));
	ASSERT_EQ(result.tooOldWithinWindow, 2);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{1, 1, 1}));
	ASSERT_EQ(result.conflicts, (std::vector<uint8_t>{0, 0, 1, 0}));
}

// Reads against random trees of several levels, in one batch of many
// transactions: with two children per node the partition of the reads
// creates more tasks than G and suspends some.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsAgainstReference") {
	for (std::size_t fanout : {std::size_t{2}, std::size_t{3}, InternalCapacity}) {
		TestTree t(2048 * BlockSize);
		std::uint32_t state = 9876 + static_cast<std::uint32_t>(fanout);
		auto next = [&state]() {
			state = state * 1103515245u + 12345u;
			return state >> 8;
		};
		// 300 leaves over wideKey(10)..wideKey(6009), with gaps between them,
		// random CVs and some single-entry leaves.
		for (unsigned i = 0; i < 300; ++i) {
			const unsigned first = 10 + 20 * i;
			std::vector<Entry> entries;
			const unsigned count = i % 4 == 2 ? 1 : 1 + next() % 6;
			for (unsigned e = 0; e < count; ++e) {
				const unsigned v = first + 2 * e;
				entries.push_back({wideKey(v), v % 4 == 0 ? keyAfter(wideKey(v)) : wideKey(v + 1),
								   static_cast<Version>(next() % 50)});
			}
			t.addLeaf(i == 0 ? Bytes{} : wideKey(first), std::move(entries));
		}
		t.build(fanout);

		for (int round = 0; round < 4; ++round) {
			const std::vector<BatchTransaction> batch =
				randomBatch(next, 1500, 6020, 50, wideKey);
			const BatchResult a = checkBatch<16>(t, batch);
			checkBatchAgainstReference(t, batch, a);
			const BatchResult b = checkBatch<32>(t, batch);
			checkBatchAgainstReference(t, batch, b);
			ASSERT(a.status == b.status);
			const auto conflicting = std::count(a.status.begin(), a.status.end(), 1);
			ASSERT(conflicting > 0 && conflicting < static_cast<long>(batch.size()));
		}
		checkStructure(t);
	}
}

// Expired entries are not read and are removed from the leaves the batch
// visits. A read whose leaf holds only expired entries before its position
// looks for the previous live entry in the leaves to its left, up to the
// first leaf.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsSkipsExpiredEntriesAcrossLeaves") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 4, 0), {30, 30, 5, 5}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 4, 0), {5, 5, 5, 30}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 4, 0), {5, 30, 30, 30}));
	t.build(2);
	Access::setReclaimCV(t.tree, 10);
	const std::vector<BatchTransaction> batch = {
		{{{key(37), key(38)}}, 20, true},
		{{{key(31), key(32)}}, 20, true},
		{{{key(36), key(37)}}, 20, true},
		{{{key(51), key(53)}}, 20, true},
		{{{key(0), key(5)}}, 20, true},
		{{{key(15), key(36)}}, 20, true},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	// expectedConflict ignores expiry: expired entries have CVs at or below
	// reclaimCV, so no read admitted at or above it conflicts with them.
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0, 0, 1, 1, 0, 0}));
	for (const Entry& e : checkStructure(t)) {
		ASSERT(e.cv > 10);
	}
}

// A leaf whose entries have all expired is emptied by the batch and
// removed from the tree after it, without changing any outcome.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsRemovesLeavesLeftEmpty") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {30, 30, 30}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {5, 5, 5}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 3, 0), {30, 30, 30}));
	t.addLeaf(key(70), withCVs(entriesFrom(70, 3, 0), {30, 30, 30}));
	t.build(2);
	Access::setReclaimCV(t.tree, 10);
	const std::vector<BatchTransaction> batch = {
		{{{key(31), key(36)}}, 20, true},
		{{{key(29), key(51)}}, 20, true},
		{{{key(33), key(34)}}, 20, true},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0, 1, 0}));
	ASSERT_EQ(Access::pendingCount(t.tree), 0u);
	const std::vector<Entry> left = checkStructure(t);
	ASSERT_EQ(left.size(), 9u);
	for (const Entry& e : left) {
		ASSERT(!(e.begin >= key(30) && e.begin < key(50)));
	}
}

// A separator marked as copying a neighbour's, because its child has no
// live entry, sends reads to the left; the leaf chain still reaches the
// entries to the right, and outcomes do not change.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsRoutesPastEmptyChildMarks") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {30, 30, 30}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {5, 5, 5}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 3, 0), {30, 30, 30}));
	t.build(3);
	Access::setReclaimCV(t.tree, 10);
	InternalBlock* root = rootNode(t);
	ASSERT(!Access::refreshSeparatorDeferred(t.tree, root, 1));
	ASSERT_EQ(root->separatorsCV[root->order[1]], Access::EmptyChildCV);
	const std::vector<BatchTransaction> batch = {
		{{{key(31), key(32)}}, 20, true},
		{{{key(36), key(51)}}, 20, true},
		{{{key(54), key(55)}}, 20, true},
		{{{key(12), key(13)}}, 20, true},
	};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0, 1, 1, 1}));
	Access::cleanPending(t.tree);
	checkStructure(t);
}

// Batches checked against hasConflict, read by read, on a tree grown by
// writes: random reads, then the batch's sorted, disjoint writes.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsMatchesHasConflictOnWrittenTree") {
	TestTree t(std::size_t{16} << 20);
	std::uint32_t state = 2468;
	auto next = [&state]() {
		state = state * 1103515245u + 12345u;
		return state >> 8;
	};
	Version now = 100;
	for (int b = 0; b < 120; ++b, now += 10) {
		std::vector<BatchTransaction> batch =
			randomBatch(next, 1 + next() % 300, 3000, 60, wideKey);
		std::vector<std::vector<uint8_t>> expected(batch.size());
		for (std::size_t tx = 0; tx < batch.size(); ++tx) {
			batch[tx].version += now - 61;
			for (const auto& [begin, end] : batch[tx].reads) {
				expected[tx].push_back(begin < end && t.hasConflict(begin, end, batch[tx].version));
			}
		}
		const BatchResult result =
			b % 2 == 0 ? checkBatch<16>(t, batch) : checkBatch<32>(t, batch);
		std::size_t i = 0;
		for (std::size_t tx = 0; tx < batch.size(); ++tx) {
			bool any = false;
			for (const uint8_t e : expected[tx]) {
				any |= e;
				if (result.conflicts[i]) {
					ASSERT(e);
				}
				if (batch[tx].report) {
					ASSERT_EQ(result.conflicts[i], e);
				}
				++i;
			}
			ASSERT_EQ(bool(result.status[tx]), any);
		}

		std::vector<unsigned> starts;
		for (int w = next() % 40; w > 0; --w) {
			starts.push_back(next() % 3000);
		}
		std::sort(starts.begin(), starts.end());
		unsigned floor = 0;
		for (unsigned s : starts) {
			if (s < floor) {
				continue;
			}
			const unsigned end = s + 1 + (next() % 5 == 0 ? next() % 100 : next() % 4);
			Access::addWrite(t.tree, asRef(wideKey(s)), asRef(wideKey(end)), now);
			floor = end;
		}
	}
	checkStructure(t);
}

namespace {

// A key of 'k', 150 filler bytes and a two-byte value: longer than a cache
// line, so that its prefetch covers two lines, and stored records span
// key blocks.
Bytes readsLongKey(unsigned value) {
	Bytes k(1, 'k');
	k.insert(k.end(), 150, static_cast<uint8_t>('a' + value % 7));
	k.push_back(static_cast<uint8_t>(value >> 8));
	k.push_back(static_cast<uint8_t>(value));
	return k;
}

} // namespace

// Reads and stored ranges with keys longer than a cache line, against
// a brute-force scan.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsWithLongKeys") {
	TestTree t(std::size_t{4} << 20);
	std::uint32_t state = 1357;
	auto next = [&state]() {
		state = state * 1103515245u + 12345u;
		return state >> 8;
	};
	// Keys sort by their filler first: build the leaves in key order.
	std::vector<unsigned> values;
	for (unsigned v = 0; v < 400; v += 2) {
		values.push_back(v);
	}
	std::sort(values.begin(), values.end(), [](unsigned a, unsigned b) {
		return readsLongKey(a) < readsLongKey(b);
	});
	std::vector<Entry> entries;
	for (unsigned v : values) {
		entries.push_back({readsLongKey(v), keyAfter(readsLongKey(v)), static_cast<Version>(next() % 50)});
	}
	for (std::size_t i = 0; i < entries.size(); i += 8) {
		std::vector<Entry> leaf(entries.begin() + i,
								entries.begin() + std::min(entries.size(), i + 8));
		const Bytes lowerBound = i == 0 ? Bytes{} : leaf.front().begin;
		t.addLeaf(lowerBound, std::move(leaf));
	}
	t.build(3);
	const std::vector<BatchTransaction> batch = randomBatch(next, 400, 400, 50, readsLongKey);
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	const auto conflicting = std::count(result.status.begin(), result.status.end(), 1);
	ASSERT(conflicting > 0 && conflicting < static_cast<long>(batch.size()));
	// Among the non-empty reads, some conflict and some do not.
	std::size_t withConflict = 0, withoutConflict = 0;
	for (const BatchTransaction& tx : batch) {
		for (const auto& [begin, end] : tx.reads) {
			if (begin < end) {
				(t.expectedConflict(begin, end, tx.version) ? withConflict : withoutConflict) += 1;
			}
		}
	}
	ASSERT(withConflict > 0 && withoutConflict > 0);
}

// Every entry has expired: below an internal root, and in a root leaf.
// Reads find no conflict, and the leaves left empty are removed afterwards.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsWithEveryEntryExpired") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {5, 5, 5}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {5, 5, 5}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 3, 0), {5, 5, 5}));
	t.build(3);
	Access::setReclaimCV(t.tree, 10);
	const std::vector<BatchTransaction> batch = {
		{{{key(31), key(36)}}, 20, true}, {{{key(55), key(58)}}, 20, true}};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0, 0}));
	ASSERT_EQ(Access::pendingCount(t.tree), 0u);
	checkStructure(t);
}

TEST_CASE("/resolver/TempConflictBTree/CheckReadsWithEveryEntryExpiredInRootLeaf") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {5, 5, 5}));
	t.build(3);
	Access::setReclaimCV(t.tree, 10);
	const std::vector<BatchTransaction> batch = {{{{key(11), key(16)}}, 20, true}};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0}));
	checkStructure(t);
}

// The previous live entry of a read lies leaves to its left, past leaves
// whose entries have all expired; a long read crosses them into live history.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsWalksLeftPastExpiredLeaves") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {30, 30, 30}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {5, 5, 5}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 3, 0), {5, 5, 5}));
	t.addLeaf(key(70), withCVs(entriesFrom(70, 3, 0), {30, 30, 30}));
	t.build(4);
	Access::setReclaimCV(t.tree, 10);
	const std::vector<BatchTransaction> batch = {
		{{{key(31), key(36)}}, 20, true}, {{{key(55), key(72)}}, 20, true}, {{{key(12), key(13)}}, 20, true}};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0, 1, 1}));
	checkStructure(t);
}

// As above, where the walk to the left leaves the read's subtree and enters
// the subtree before it.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsWalksLeftIntoPreviousSubtree") {
	TestTree t;
	for (unsigned i = 0; i < 8; ++i) {
		const unsigned first = 10 + 20 * i;
		const Version cv = i < 4 ? 50 : i < 7 ? 5 : 40;
		t.addLeaf(i == 0 ? Bytes{} : key(first), withCVs(entriesFrom(first, 2, 0), {cv, cv}));
	}
	t.build(4);
	Access::setReclaimCV(t.tree, 10);
	const std::vector<BatchTransaction> batch = {
		{{{key(151), key(152)}}, 20, true}, {{{key(71), key(155)}}, 20, true}, {{{key(150), key(151)}}, 45, true}};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{0, 1, 0}));
	checkStructure(t);
}

// The empty key as the begin of a stored range and of reads.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsWithEmptyKeys") {
	TestTree t;
	t.addLeaf({}, {{Bytes{}, key(5), 30}, {key(8), key(9), 30}, {key(10), key(11), 30}});
	t.addLeaf(key(30), entriesFrom(30, 3, 30));
	t.build(3);
	const std::vector<BatchTransaction> batch = {
		{{{Bytes{}, key(3)}}, 20, true}, {{{Bytes{}, key(40)}}, 40, true}, {{{key(6), key(7)}}, 20, true}};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{1, 0, 0}));
	checkStructure(t);
}

// A stored general range whose begin fills more than a key block, so that
// its end starts in a later block, and reads that begin inside it or past it.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsWithRangeEndInLaterKeyBlock") {
	TestTree t(2048 * BlockSize);
	Bytes begin = key(20);
	begin.resize(2100, 'x');
	Bytes end = key(21);
	end.resize(140, 'y');
	t.addLeaf({}, {{key(10), key(11), 30}, {begin, end, 30}, {key(40), key(41), 30}});
	t.build(3);
	Bytes readBegin = key(20);
	readBegin.resize(2100, 'z');
	const std::vector<BatchTransaction> batch = {
		{{{readBegin, key(30)}}, 20, true}, {{{key(22), key(30)}}, 20, true}};
	const BatchResult result = checkBatch<16>(t, batch);
	checkBatchAgainstReference(t, batch, result);
	ASSERT_EQ(result.status, (std::vector<uint8_t>{1, 0}));
}

// addWrites with several intervals per call, against a model of every write:
// a read conflicts when it overlaps a write with a CV above its version.
// Results are read back through checkReads only.
TEST_CASE("/resolver/TempConflictBTree/AddWritesAgainstReference") {
	TestTree t(2048 * BlockSize);
	std::uint32_t state = 4242;
	auto next = [&state]() {
		state = state * 1103515245u + 12345u;
		return state >> 8;
	};
	struct Write {
		unsigned begin, end;
		Version cv;
	};
	std::vector<Write> model;
	Version cv = 10;
	for (int call = 0; call < 60; ++call, cv += 3) {
		// Sorted, disjoint intervals over keys 0 to 239; some adjacent.
		std::vector<std::pair<Bytes, Bytes>> owned;
		unsigned position = next() % 8;
		while (position < 236 && owned.size() < 12) {
			const unsigned length = 1 + next() % (next() % 5 == 0 ? 40 : 4);
			const unsigned last = std::min(position + length, 239u);
			owned.emplace_back(key(position), key(last));
			model.push_back({position, last, cv});
			position = last + next() % 12;
		}
		std::vector<std::pair<StringRef, StringRef>> writes;
		for (const auto& [b, e] : owned) {
			writes.emplace_back(asRef(b), asRef(e));
		}
		t.tree.addWrites(std::span<const std::pair<StringRef, StringRef>>(writes.data(), writes.size()), cv);
		ASSERT_EQ(t.tree.reclaimCV(), 0);

		std::vector<BatchTransaction> batch(40);
		for (BatchTransaction& tx : batch) {
			const unsigned b = next() % 240;
			const unsigned e = std::min(b + 1 + next() % (next() % 4 == 0 ? 60 : 3), 240u);
			tx.reads.emplace_back(key(b), key(e));
			tx.version = 10 + static_cast<Version>(next() % (cv - 7));
			tx.report = true;
		}
		const BatchResult result = checkBatch<16>(t, batch);
		std::size_t conflicting = 0;
		for (std::size_t i = 0; i < batch.size(); ++i) {
			const Bytes& b = batch[i].reads[0].first;
			const Bytes& e = batch[i].reads[0].second;
			bool expected = false;
			for (const Write& w : model) {
				expected |= key(w.begin) < e && b < key(w.end) && w.cv > batch[i].version;
			}
			ASSERT_EQ(bool(result.status[i]), expected);
			ASSERT_EQ(bool(result.conflicts[i]), expected);
			conflicting += expected;
		}
		if (call >= 5) {
			ASSERT(conflicting > 0 && conflicting < batch.size());
		}
	}
}

namespace {

// Builds nine leaves of three entries, three per internal node, where the
// leaves of subtree `expired` (0 or 1) hold entries at CV 5 and the others at
// CV 30. The expired ranges and the keys of the separators that will lead to
// them are stored first, at CV 5, with filler at CV 5 up to the end of their
// key block, so that this oldest block holds nothing else. After build, the
// separators of the expired subtree, in its parent and in itself, are those
// keys. Then the reclaim cut moves to 10, the oldest key block is reclaimed,
// and keys are appended until the pool hands that block out again: those
// separators now point at other bytes.
void buildWithSeparatorsInReusedKeyBlock(TestTree& t, unsigned expired) {
	auto appendKey = [&t](const Bytes& k, Version keyCV) {
		const std::optional<RangeRef> ref = t.keys().appendKey(
			reinterpret_cast<const std::byte*>(k.data()), static_cast<std::uint16_t>(k.size()), keyCV);
		ASSERT(ref.has_value());
		return *ref;
	};
	auto lowerBound = [](unsigned leaf) { return leaf == 0 ? Bytes{} : key(10 + 20 * leaf); };
	const unsigned firstExpired = 3 * expired;
	std::vector<std::vector<Entry>> expiredEntries;
	std::vector<std::vector<RangeRef>> expiredRanges;
	std::vector<RangeRef> childSeparators;
	for (unsigned i = firstExpired; i < firstExpired + 3; ++i) {
		expiredEntries.push_back(entriesFrom(10 + 20 * i, 3, 5));
		expiredRanges.emplace_back();
		for (const Entry& e : expiredEntries.back()) {
			const std::optional<RangeRef> range = t.keys().appendRange(asRef(e.begin), asRef(e.end), e.cv);
			ASSERT(range.has_value());
			expiredRanges.back().push_back(*range);
		}
		childSeparators.push_back(appendKey(lowerBound(i), 5));
	}
	const RangeRef parentSeparator = appendKey(lowerBound(firstExpired), 5);
	const KeyBlockHeader* oldestBlock = t.keys().keyBlock(expiredRanges[0][0]);
	for (unsigned n = 0; t.keys().keyBlock(appendKey(key(200 + n % 50), 5)) == oldestBlock; ++n) {
	}

	for (unsigned i = 0; i < 9; ++i) {
		if (i >= firstExpired && i < firstExpired + 3) {
			t.addLeaf(lowerBound(i), expiredEntries[i - firstExpired], expiredRanges[i - firstExpired]);
		} else {
			t.addLeaf(lowerBound(i), entriesFrom(10 + 20 * i, 3, 30));
		}
	}
	t.build(3);
	auto* root = reinterpret_cast<InternalBlock*>(Access::root(t.tree));
	ASSERT_EQ(root->header.count, 3u);
	auto* subtree = reinterpret_cast<InternalBlock*>(root->children[root->order[expired]]);
	ASSERT_EQ(subtree->header.count, 3u);
	root->separators[root->order[expired]] = parentSeparator;
	for (std::size_t r = 0; r < 3; ++r) {
		subtree->separators[subtree->order[r]] = childSeparators[r];
		ASSERT_EQ(subtree->separatorsCV[subtree->order[r]], 5);
	}

	Access::setReclaimCV(t.tree, 10);
	ASSERT_EQ(t.keys().reclaimOldest(), std::optional<Version>(7));
	ASSERT(!t.keys().isValid(childSeparators[1]));
	bool reused = false;
	for (std::size_t n = 0; n < BlockSize && !reused; ++n) {
		reused = t.keys().keyBlock(appendKey(key(n % 250), 40)) == oldestBlock;
	}
	ASSERT(reused);
}

} // namespace

// Regression: an expired separator may point into a key block that was
// reclaimed and reused, so its key must not be read. A long read that
// starts in a subtree whose entries have all expired must still reach the
// live history after it. As a middle child, the expired subtree is routed
// past through its parent's separator; as the first child, reads enter it
// and meet its own expired separators, none of them with a live child.
TEST_CASE("/resolver/TempConflictBTree/CheckReadsPastSeparatorsInReusedKeyBlock") {
	{
		TestTree t;
		buildWithSeparatorsInReusedKeyBlock(t, 1);
		const std::vector<BatchTransaction> batch = {
			{{{key(75), key(135)}}, 20, true},  // from the expired subtree into leaf 6
			{{{key(91), key(95)}}, 20, true},   // inside the expired subtree
			{{{key(60), key(80)}}, 20, true},   // from leaf 2 into the expired subtree
			{{{key(12), key(13)}}, 20, true},   // leaf 0
			{{{key(111), key(200)}}, 40, true}, // newer than every entry
		};
		const BatchResult result = checkBatch<16>(t, batch);
		checkBatchAgainstReference(t, batch, result);
		ASSERT_EQ(result.status, (std::vector<uint8_t>{1, 0, 0, 1, 0}));
		ASSERT_EQ(Access::pendingCount(t.tree), 0u);
	}
	{
		TestTree t;
		buildWithSeparatorsInReusedKeyBlock(t, 0);
		const std::vector<BatchTransaction> batch = {
			{{{key(35), key(75)}}, 20, true},   // from the expired subtree into leaf 3
			{{{key(31), key(36)}}, 20, true},   // inside the expired subtree
			{{{key(12), key(13)}}, 20, true},   // its first leaf
			{{{key(72), key(73)}}, 20, true},   // leaf 3
			{{{key(51), key(200)}}, 40, true},  // newer than every entry
		};
		const BatchResult result = checkBatch<16>(t, batch);
		checkBatchAgainstReference(t, batch, result);
		ASSERT_EQ(result.status, (std::vector<uint8_t>{1, 0, 0, 1, 0}));
		ASSERT_EQ(Access::pendingCount(t.tree), 0u);
	}
}
