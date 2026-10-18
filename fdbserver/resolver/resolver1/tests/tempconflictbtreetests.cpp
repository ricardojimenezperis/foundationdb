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

	static bool findKey(const TempConflictBTree& t, const StringRef& key,
						LeafBlock*& leaf, std::size_t& rank) {
		return t.findKey(key, leaf, rank);
	}
	static bool nextRank(const TempConflictBTree& t, LeafBlock*& leaf,
						 std::size_t& rank, std::size_t& slot) {
		return t.nextRank(leaf, rank, slot);
	}
	static bool prevRank(const TempConflictBTree& t, LeafBlock*& leaf,
						 std::size_t& rank, std::size_t& slot) {
		return t.prevRank(leaf, rank, slot);
	}
	static bool hasConflict(const TempConflictBTree& t,
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
			Version cv;     // CV of the first entry below, as refreshing sets it
		};
		std::vector<Child> level;
		for (const LeafModel& l : leaves) {
			level.push_back({&l.leaf->header, l.lowerBound,
							 l.entries.empty() ? 0 : l.entries.front().cv});
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
	const bool found = Access::findKey(t.tree, asRef(k), leaf, rank);

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
	ASSERT(!Access::findKey(t.tree, asRef(key(1)), leaf, rank));
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
	ASSERT(!Access::findKey(t.tree, asRef(key(25)), leaf, rank));
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
		ASSERT_EQ(slot, leaf->order[rank]);
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
	ASSERT_EQ(slot, leaf->order[1]);

	leaf = t.leaves[3].leaf;
	rank = 0;
	ASSERT(Access::nextRank(t.tree, leaf, rank, slot));
	ASSERT(leaf == t.leaves[5].leaf);
	ASSERT_EQ(rank, 0);
	ASSERT_EQ(slot, leaf->order[0]);

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
	ASSERT(!Access::findKey(t.tree, asRef(key(5)), leaf, rank));
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
		const auto slot = leaf->order[r];
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
	for (std::size_t r = 0; r < internal->header.count; ++r) {
		const auto slot = internal->order[r];
		ASSERT(internal->children[slot]->parent == node);
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
			for (Version version : {Version{0}, Version{15}, Version{40}}) {
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
	ASSERT_EQ(leaf->freeSlots, LeafSlotMask & ~((std::uint64_t{1} << leaf->order[0]) |
											   (std::uint64_t{1} << leaf->order[1])));

	ASSERT(!Access::cleanExpired(t.tree, leaf));
	checkTree(t);
}

TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorKeepsFreshSeparator") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {5, 6, 7}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {8, 2, 9}));
	t.build(3);
	InternalBlock* root = rootNode(t);
	const KeyRef before = root->separators[root->order[1]];

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
	ASSERT(t.keys().isValid(root->separators[slot]));
	ASSERT_EQ(leafEntries(t, t.leaves[1].leaf).size(), 1);
	checkTree(t);
}

// A leaf left empty is removed, its neighbours are linked to each other and
// the parent's count of children needing a split is updated.
TEST_CASE("/resolver/TempConflictBTree/RefreshSeparatorRemovesEmptyLeaf") {
	TestTree t;
	t.addLeaf({}, withCVs(entriesFrom(10, 3, 0), {20, 20, 20}));
	t.addLeaf(key(30), withCVs(entriesFrom(30, 3, 0), {5, 10, 8}));
	t.addLeaf(key(50), withCVs(entriesFrom(50, 3, 0), {20, 20, 20}));
	t.build(3);
	InternalBlock* root = rootNode(t);
	t.leaves[1].leaf->header.needsSplit = true;
	root->header.childrenNeedingSplit = 1;

	Access::setReclaimCV(t.tree, 10);
	ASSERT(Access::refreshSeparator(t.tree, root, 1));

	ASSERT_EQ(root->header.count, 2);
	ASSERT_EQ(root->header.childrenNeedingSplit, 0);
	ASSERT(childAt(root, 1) == &t.leaves[2].leaf->header);
	ASSERT(t.leaves[0].leaf->right == t.leaves[2].leaf);
	ASSERT(t.leaves[2].leaf->left == t.leaves[0].leaf);
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
	const std::vector<Entry> entries = checkTree(t);
	ASSERT_EQ(entries.size(), 6);
	ASSERT_EQ(leafEntries(t, t.leaves[2].leaf).size(), 3);

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
	ASSERT_EQ(root->freeSlots, InternalSlotMask);
}
