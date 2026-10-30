# TempConflictBTree validation

## Scope

Validation of the temporal conflict B+ tree: a history of write-conflict
ranges ordered by key, each with the commit version (CV) of the write that
stored it, kept in nodes from `BNodeStore` and keys from `KeyStore`, both
drawing blocks from one `PagePool`. The following are validated:

- Search: descent through internal nodes to the leaf and rank of a key,
  including keys before the first separator, and movement between entries
  across leaves (`findKey`, `nextRank`, `prevRank`).
- Writes (`addWrites`, and `addWrite` for each range): insertion of a range
  at one CV, keeping the parts of overlapped ranges outside it (left and
  right exteriors) with their original CVs, erasure of the ranges it covers
  across leaves and whole subtrees, creation of the root leaf, proactive
  splitting on the way down, so that the leaf that receives a write always
  has two free slots, and the room reserved before each write, with the
  `std::length_error` thrown when the pool cannot hold one write.
- Structure maintenance: leaf and internal splits (the internal cut by
  weight, counting twice the children that need a split), insertion of
  siblings and new roots, the per-node count of children needing a split,
  propagation of first keys, CVs and split state to the ancestors, erasure
  of entries and children with removal of nodes left empty, collapse of the
  root, and the links between leaves.
- Expiry: entries and separators with a CV at or below `reclaimCV_` are
  treated as expired. Expired entries are removed without reading their
  keys, expired separators are refreshed from the first live key below
  them, and children without live entries are removed.
- Reclamation by capacity (`reclaimOldest`, `reclaimUntilAvail`): the
  oldest age of nodes or key blocks is released, whole subtrees whose
  maximum CV is that age are detached and released, and the cut
  `reclaimCV_` never moves back.
- The batch read check `checkReads<G>`, for the task capacities G = 16 and
  G = 32: reads taken from a batch laid out as FoundationDB's
  `ConflictBatch` lays it out, too-old transactions, empty reads,
  transactions already conflicting, reporting transactions with one
  conflict bit per read, partition of the reads among children, tasks
  suspended beyond G and resumed, compaction of each leaf once per batch,
  refresh of expired separators before their keys are used, children
  without live entries marked with `EmptyChildCV` and routed conservatively
  to the left, the search for a previous live entry in the leaves to the
  left, the empty key, and removal after the traversal of the leaves left
  empty.
- The single-read check `hasConflict`, a private function that the tests
  use as a reference for `checkReads`.

Outside the scope of this component: `PagePool`, `KeyStore` and
`BNodeStore` themselves (see [PagePoolTests.md](PagePoolTests.md),
[KeyStoreTests.md](KeyStoreTests.md) and
[BNodeStoreTests.md](BNodeStoreTests.md)); the integration of the tree in
the resolver (`ConflictSet.cpp`) and the construction of the batch there;
the benchmark in `bench/`; performance and the effect of the prefetch hints
(`__builtin_prefetch` is a hint, and no result depends on it); and
concurrent access. `fdbcompat.h` provides stand-ins for FoundationDB types
so that the tree builds without `flow`, and `../ResolverBatchTypes.h`
defines the batch records (`KeyInfo`, `TransactionInfo`,
`ReadConflictRange`) shared with FoundationDB; neither is measured as part
of this component.

### Interface contract

`checkReads<G>` takes the batch as FoundationDB's `ConflictBatch` builds it.
Its contract, stated on its declaration in `tempconflictbtree.h`, and the
layout it reads:

- `points` holds the begin and end of every read range, and of the write
  ranges if any, sorted by key. The range index of a point is derived from
  its `pIndex`, which points into the `readRanges` or `writeRanges` array of
  its transaction. The reads of transaction `t` are consecutive in `reads`,
  starting at `offsets[t]`. `transactionInfo` and
  `transactionConflictStatus` have one entry per transaction.
- The records and key bytes of the input stay valid for the call.
- No write and no key-block reclamation take place during the traversal.
- G is 16 or 32, the two capacities instantiated in
  `tempconflictbtree.cpp`.

Its effect: `conflicts` is reset to one entry per read; conflicting
transactions have their flag set and no flag is cleared. A read whose
version is below `reclaimCV_` marks its transaction too old, with its flag
set, and `tooOldWithinWindow` counts each transaction that becomes too old
once. A transaction already marked conflicting is checked again only if it
has a reporting pointer (`conflictingKeyRange`), and then each of its
conflicting reads has its bit set. Empty reads (begin not below end) check
nothing. The call may refresh separators, remove expired entries and remove
empty nodes; it adds no writes and reclaims no key blocks.

`addWrites` takes sorted, disjoint write ranges, all with one CV, which
must not decrease between calls. Each range has its begin below its end,
which `KeyStore::appendRange` requires, and keys of at most 65,535 bytes.
Before each range, `addWrite` reclaims history until the pool has
`max(35, 3 * ceil(2L / P) + 2)` free blocks, where `L` is the longest key
stored so far or of this range and `P` is the payload of a key block
(`BlockSize - sizeof(KeyBlockHeader)`, 2,024 bytes): room for the range and
two exteriors, each a record of two keys, and for two nodes. If, after
reclaiming everything it can, the pool has fewer than
`3 * ceil(2L / P) + 2` free blocks, it throws `std::length_error`: the
pool cannot hold one write. The reclamation already done stays, the range
is not stored, and the ranges stored earlier in the same call remain. `L`
does not decrease when long keys are reclaimed: after one long key, every
later write keeps its reserve (see
[Reserve after a long key](#reserve-after-a-long-key)).

Expiry relies on reclaimed key blocks having a CV at or below `reclaimCV_`,
which `reclaimOldest` maintains. Several tests set `reclaimCV_` directly
through the test access, without reclaiming key blocks, which keeps this
requirement; one test reclaims a key block directly after raising the cut
above its CV. The header also states that no reclamation may occur while
key bytes are being accessed.

## Source and environment

- Source commit: the tested files are identified by content; this report
  is committed together with them. SHA-256 (first 16 hex digits):

  | File | SHA-256 |
  |---|---|
  | `tempconflictbtree.cpp` | `879b0d901a776403` |
  | `tempconflictbtree.h` | `5973f13897edf55a` |
  | `bnodestore.cpp` | `a945a23136d881e9` |
  | `bnodestore.h` | `4ff857dd23b118d6` |
  | `keystore.cpp` | `b09886264fe0237c` |
  | `keystore.h` | `5669791aadb07001` |
  | `pagepool.cpp` | `1b463f87266e646f` |
  | `pagepool.h` | `54456873974dbb8f` |
  | `fdbcompat.h` | `2e57a5fd8b1629ea` |
  | `common.h` | `a815af89662b3a73` |
  | `../ResolverBatchTypes.h` | `44e93cb91d982c40` |
  | `test.h` | `a9a025cd287fd82e` |
  | `testmain.cpp` | `f977afc8cc420126` |
  | `makefile` | `2a8173902bc33339` |
  | `tests/tempconflictbtreetests.cpp` | `8c30706df0e9ea8c` |
  | `tests/pagepooltests.cpp` | `059580ab0c6dff71` |
  | `tests/keystoretests.cpp` | `4490adc5be7f9623` |
  | `tests/bnodestoretests.cpp` | `eba26821ec6756c6` |
  | `tests/coverage.sh` | `6efaddf0828ff825` |

- Working tree: the files listed above
- Platform: macOS 27.0.1 (build 26A434), arm64 (Apple M3 Pro)
- Compiler: Apple clang 15.0.0 (clang-1500.3.9.4), target
  `arm64-apple-darwin27.0.0`
- Language standard: C++20
- Build flags: `-I. -std=c++20 -Wall -Wextra -Wpedantic -g`, with the
  default block size of 2048 bytes (`RESOLVER1_BLOCK_SIZE` not defined).
  With this block size `LeafCapacity` is 59 and `InternalCapacity` is 40.
- Coverage tools and versions:
  - `gcov` (Apple LLVM version 15.0.0, `llvm-cov gcov` compatible mode)
  - `llvm-cov` and `llvm-profdata` (Apple LLVM version 15.0.0, via `xcrun`)
- Coverage build flags:
  - gcov: `-I. -std=c++20 -Wall -Wextra -Wpedantic -O0 --coverage`
  - llvm-cov: `-I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=.`

The build of the full suite reports no warnings.

## Reproduction

Run from the project root:

```bash
make test
```

To run only this component's tests:

```bash
./tests.exe /resolver/TempConflictBTree
```

Coverage collection with gcov:

```bash
rm -rf build/coverage && mkdir -p build/coverage
FLAGS="-I. -std=c++20 -Wall -Wextra -Wpedantic -O0 --coverage"
for f in pagepool keystore bnodestore tempconflictbtree testmain tests/pagepooltests tests/keystoretests tests/bnodestoretests tests/tempconflictbtreetests; do
	g++ $FLAGS -c $f.cpp -o build/coverage/$(basename $f).o
done
g++ --coverage build/coverage/*.o -o build/coverage/tests.exe
(cd build/coverage && ./tests.exe /resolver/TempConflictBTree)
gcov -b -c -o build/coverage/tempconflictbtree.o tempconflictbtree.cpp > build/coverage/tempconflictbtree-coverage.txt 2>&1
mv *.gcov build/coverage/
```

Coverage collection with llvm-cov:

```bash
rm -rf build/coverage-llvm && mkdir -p build/coverage-llvm
g++ -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=. pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp testmain.cpp tests/*.cpp -o build/coverage-llvm/tests.exe
LLVM_PROFILE_FILE=build/coverage-llvm/tempconflictbtree.profraw build/coverage-llvm/tests.exe /resolver/TempConflictBTree
xcrun llvm-profdata merge -o build/coverage-llvm/tempconflictbtree.profdata build/coverage-llvm/tempconflictbtree.profraw
xcrun llvm-cov report build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/tempconflictbtree.profdata '-ignore-filename-regex=(pagepool|keystore|bnodestore|fdbcompat|ResolverBatchTypes|test)' > build/coverage-llvm/tempconflictbtree-llvm-report.txt
xcrun llvm-cov show build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/tempconflictbtree.profdata -show-branches=count '-ignore-filename-regex=(pagepool|keystore|bnodestore|fdbcompat|ResolverBatchTypes|test)' > build/coverage-llvm/tempconflictbtree-llvm-show.txt
```

Only the TempConflictBTree tests are run. Each collection starts from an
empty directory, so counters from earlier runs are not merged in. The
`-ignore-filename-regex` option leaves only `tempconflictbtree.cpp` and
`tempconflictbtree.h` in the llvm-cov output.

`tests/coverage.sh` runs these commands for the four components, after the
full suite whose output is the test evidence. Run it from the project root:

```bash
bash tests/coverage.sh
```

A second run of the gcov commands, in a separate copy of the files listed
above, gave the same annotated gcov file.

## Test cases

The tests reach private members through `TempConflictBTreeTestAccess`, a
friend of the tree. Most tests build a tree directly from nodes: leaves
with given entries and lower bounds, then internal levels of a given
fanout, with each separator the lower bound of its first leaf. Entries are
inserted at rank 0 in reverse order, so that the order of a node is not the
identity. Unless stated otherwise the pool budget is 1 MiB (16 pages of
64 KiB, 512 blocks of 2 KiB), and keys are `k` followed by one byte.

Three checks recur:

- **Structure**: every child points to its parent with its slot; each
  internal node's count of children needing a split matches its children;
  `maxSubtreesCV` of each slot and the node's `maxCV` are at least the
  child's `maxCV`; only a root leaf is empty; entries are in key order and
  disjoint; every begin in a child is at or above its separator and every
  begin before it below it, except for separators whose key has been
  reclaimed, which must have expired; leaves are linked left to right in
  key order. Entries whose key block has been reclaimed are skipped
  without reading them, and must have expired.
- **Tree**: the structure check, and `hasConflict` for every read
  `[k(b), k(e))` with b from 0 to 230 in steps of 3 and e from b + 1 to 231
  in steps of 7, at versions `reclaimCV`, 0, 15 and 40 when not below
  `reclaimCV`, against a brute-force scan of the entries left in the tree.
- **Batch reference**: a batch is laid out as `ConflictBatch` lays it out
  (reads of a transaction consecutive, endpoints sorted with a stable
  sort), and the result of `checkReads` is checked against a brute-force
  scan of the entries: a transaction is marked when it was already marked
  or when one of its non-empty reads conflicts; no read is marked without a
  conflict; in a reporting transaction not already marked, each read's bit
  equals its expected conflict; no transaction becomes too old. Random
  batches mix points, short ranges, long ranges, empty ranges and repeated
  reads, in transactions of one to four reads, a third of them reporting.

Search and navigation:

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/FindKeyInEmptyTree` | In an empty tree `findKey` returns false, a null leaf and rank 0. | PASS |
| `/resolver/TempConflictBTree/FindKeyInSingleLeaf` | In a root leaf of six entries, every key from `k(0)` to `k(30)`, each key followed by a zero byte, the empty key, `k` and `l` reach the leaf with the rank and found flag of a reference search. | PASS |
| `/resolver/TempConflictBTree/FindKeyThroughOneInternalLevel` | The same through one internal level over three leaves. | PASS |
| `/resolver/TempConflictBTree/FindKeyThroughTwoInternalLevels` | The same through two internal levels over seven leaves. | PASS |
| `/resolver/TempConflictBTree/NextRankVisitsEveryEntry` | Over leaves of 3, 1, 2, 1, 1 and 4 entries under two internal nodes, `nextRank` visits every entry in key order with its slot; after the last it returns false, sets the rank to the end position and leaves leaf and slot unchanged. | PASS |
| `/resolver/TempConflictBTree/PrevRankVisitsEveryEntry` | From the end position of the last leaf, `prevRank` visits every entry in reverse order; before the first it returns false and changes nothing. | PASS |
| `/resolver/TempConflictBTree/RankMovesBetweenLeaves` | From the first entry of a leaf both directions cross to the neighbouring leaves, also across internal nodes; from the end position `nextRank` goes right; an empty root leaf has no entry in either direction. | PASS |
| `/resolver/TempConflictBTree/FindKeyBeforeFirstSeparator` | With a first separator above the smallest key, keys below it reach the first leaf at rank 0, and every other search key reaches the reference position. | PASS |
| `/resolver/TempConflictBTree/NewTreeHasNoReclaimedHistory` | A new tree has `reclaimCV` 0. | PASS |

Single-read check:

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/HasConflictInEmptyTree` | An empty tree has no conflict. | PASS |
| `/resolver/TempConflictBTree/HasConflictAroundOneRange` | Against `[k(20), k(30))` with CV 10: overlapping reads conflict only at versions below 10; reads touching either end without overlapping do not, since ranges are half-open. | PASS |
| `/resolver/TempConflictBTree/HasConflictWithPoint` | A stored point `[k, k\0)` conflicts only with reads containing k. | PASS |
| `/resolver/TempConflictBTree/HasConflictAgainstReference` | With fanouts 2 and 3, ten leaves with gaps, random CVs and some single-entry leaves; reads between pairs of search keys, including the empty key, at random versions agree with a brute-force scan; some conflict and some do not. | PASS |

Expiry, separator refresh and deferred cleanup:

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/CleanExpiredRemovesOnlyExpiredEntries` | With `reclaimCV` 10, cleaning a leaf with CVs 5, 15, 3, 20 and 10 keeps exactly the entries with CVs 15 and 20, frees exactly the other slots and reports a change; a second cleaning reports none. | PASS |
| `/resolver/TempConflictBTree/RefreshSeparatorKeepsFreshSeparator` | A separator whose CV is above `reclaimCV` is not changed, and its leaf is not cleaned. | PASS |
| `/resolver/TempConflictBTree/RefreshSeparatorFromLeaf` | An expired separator becomes the begin of the first entry left after cleaning the leaf, with its CV and a valid key. | PASS |
| `/resolver/TempConflictBTree/RefreshSeparatorRemovesEmptyLeaf` | A full leaf left empty by the refresh is removed; its neighbours are linked and the parent no longer counts it as needing a split. | PASS |
| `/resolver/TempConflictBTree/CleanExpiredPropagatesSplitCleared` | A full leaf under a parent with one free slot gets two free slots: the leaf, its parent and the root stop counting a split. | PASS |
| `/resolver/TempConflictBTree/CleanExpiredKeepsSplitCountsOfNonFullLeaf` | Cleaning a leaf that did not need a split leaves the counts unchanged, even when it empties it. | PASS |
| `/resolver/TempConflictBTree/RemoveEmptyChildPropagatesSplitCleared` | Removing an empty leaf frees a slot of a parent that needed a split: the parent stops needing it and the root stops counting it. | PASS |
| `/resolver/TempConflictBTree/RefreshSeparatorSkipsExpiredGrandchildren` | Refreshing the separator of an internal child whose first two leaves have expired removes both and takes the separator, with its CV, from the third. | PASS |
| `/resolver/TempConflictBTree/RefreshSeparatorRemovesEmptyInternalChild` | When every leaf under the internal child has expired, the child is removed too. | PASS |
| `/resolver/TempConflictBTree/RefreshSeparatorsRefreshesWholeNode` | Refreshing every separator of a node removes the leaves left empty and leaves every separator above `reclaimCV`; a leaf whose separator has not expired is not cleaned; a second pass changes nothing. | PASS |
| `/resolver/TempConflictBTree/RefreshSeparatorsCanEmptyNode` | When every child has expired, refreshing leaves the node with no children and every slot free. | PASS |
| `/resolver/TempConflictBTree/DeferredRefreshKeepsExpiredEntries` | The deferred refresh sets the separator to the first live entry without removing the expired ones, queues the leaf once, and does not visit it again; cleaning the queue then removes the expired entries. | PASS |
| `/resolver/TempConflictBTree/DeferredRefreshMarksEmptyLeaf` | A leaf without live entries stays linked, its separator marked `EmptyChildCV` with the key of the child before it, and is not visited again; cleaning the queue removes it and relinks its neighbours. | PASS |
| `/resolver/TempConflictBTree/DeferredRefreshMarksLeadingChildren` | Refreshing the first separator when the first two children have no live entry marks both with the key of the first live child, whose separator is refreshed; three leaves are queued, and cleaning removes the two empty ones. | PASS |
| `/resolver/TempConflictBTree/DeferredRefreshThroughInternalChild` | The refresh goes down through an internal child, whose leading children without live entries are marked; cleaning the queue removes them. | PASS |
| `/resolver/TempConflictBTree/DeferredRefreshMarksEmptyInternalChild` | An internal child without live entries is marked; cleaning the queue removes its leaves and then the child. | PASS |
| `/resolver/TempConflictBTree/FirstLiveKey` | `firstLiveKey` of the root returns the smallest live key with its CV; when every entry has expired it returns nothing, and cleaning the queue empties the tree. | PASS |
| `/resolver/TempConflictBTree/CleanPendingSkipsReleasedLeaf` | A leaf queued twice is removed once: the second record is rejected by its generation. | PASS |
| `/resolver/TempConflictBTree/CleanExpiredReportsLeafTransitionOnce` | For leaves of 57 to 59 entries and cuts removing 0 to 57 of them, cleaning reports the split transition only from at most one free slot to at least two, through a parent with one free slot up to the root. | PASS |
| `/resolver/TempConflictBTree/FindKeyRemovesExpiredLeaf` | A search into a leaf whose entries have all expired removes it on the way: the search ends after the last entry of its left neighbour, the neighbours are linked and the next separator is kept. | PASS |
| `/resolver/TempConflictBTree/FindKeyDropsCandidateClearedBelow` | An internal node that needed a split on the way down stops needing it when cleaning the leaf below frees slots; `findKey` then returns no split candidate. | PASS |
| `/resolver/TempConflictBTree/HasConflictAfterFirstLeafRemoved` | With the first leaf expired and removed by the search, a read starting below every remaining key has no previous entry and is checked against the ranges to its right. | PASS |
| `/resolver/TempConflictBTree/HasConflictInExpiredRootLeaf` | A root leaf whose entries have all expired is emptied by the search, stays the root, and the read finds no conflict. | PASS |

Splits:

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/NeedsSplitOfHeaderMatchesNodeKind` | For many free-slot patterns, a leaf needs a split with at most one free slot and an internal node when its free slots equal its children needing a split; the header overload agrees with the typed ones. | PASS |
| `/resolver/TempConflictBTree/FindKeyReturnsDeepestNodeNeedingSplit` | In a layout of 25 leaves under a first internal node that needs a split, 15 of them full, and 5 leaves under a second node, `findKey` returns the full leaf, otherwise the internal node needing a split, otherwise none. | PASS |
| `/resolver/TempConflictBTree/SplitRootLeaf` | A full root leaf splits into halves of 29 and 30 entries under a new root with CV 500, linked to each other; the new separator is the first begin of the sibling with its CV. For every begin, every key after a begin and a key before the first, the leaf and rank returned by the split are those `findKey` returns on the split tree, and no entry is lost. | PASS |
| `/resolver/TempConflictBTree/SplitLeafUnderParent` | A full leaf under a parent: the sibling is linked between the leaf and its right neighbour and inserted after it in the parent, which stops counting the leaf; positions are kept as above. | PASS |
| `/resolver/TempConflictBTree/SplitLeafUnderParentNeedingSplit` | Splitting a full leaf whose parent needs a split uses the reserved slot: the parent becomes full, counts no child, and is now the node `findKey` returns as needing a split. | PASS |
| `/resolver/TempConflictBTree/SplitRootInternal` | Five layouts of an internal root needing a split (full without counted children; counted children first, last and spread; all counted): the cut is the shortest prefix reaching half the weight, both halves stop needing a split, children keep their order, the sibling's slots are filled in rank order, and leaf and rank are unchanged. | PASS |
| `/resolver/TempConflictBTree/SplitInternalUnderParent` | An internal node under the root: the sibling goes right after it, the root stops counting it, the counted children are shared between the halves, and the sibling's CV reaches the root; every leaf is still reached, and only full leaves are returned as needing a split. | PASS |
| `/resolver/TempConflictBTree/LeafNeedsSplitWithOneFreeSlot` | Leaves with 57, 58 and 59 entries: only the last two need a split, are counted by the parent and are returned by `findKey`. | PASS |
| `/resolver/TempConflictBTree/SplitLeafWithOneFreeSlot` | A leaf with one free slot splits into halves of 29 entries that do not need a split, and every position is kept. | PASS |

Insertion, erasure, detachment and propagation:

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/InsertAtFillsRootLeaf` | An empty root leaf is filled in a scattered order: each insertion raises the leaf's maximum CV, the leaf stays the oldest node, and it needs a split from its 58th entry on. | PASS |
| `/resolver/TempConflictBTree/InsertAtFillsLeafUnderParent` | A leaf between two others is filled in a scattered order: its separator follows its first key, the parent's CVs follow the leaf's, and the parent counts the leaf from its 58th entry on. | PASS |
| `/resolver/TempConflictBTree/InsertAtUpdateCombinations` | All eight combinations of the updates an insertion may cause in the parent: the leaf starts needing a split, the separator changes because the entry goes first, the maximum CV rises because the entry is newer. | PASS |
| `/resolver/TempConflictBTree/InsertAtPropagatesSeparator` | An entry inserted first in a leaf changes the separators up to the first ancestor where the subtree is not the first child, for first and non-first leaves of both internal nodes. | PASS |
| `/resolver/TempConflictBTree/InsertAtPropagatesNeedsSplit` | A leaf that starts needing a split under a parent with one free slot makes the parent need one, and the root counts it; a further entry that fills the leaf changes no count. | PASS |
| `/resolver/TempConflictBTree/InsertAtPropagatesMaxCV` | A newer entry raises the maximum CVs up to the root; a second one with the same CV under the same parent stops at the parent. | PASS |
| `/resolver/TempConflictBTree/PropagateUpdatesSeparator` | After removing the first entry of a leaf, propagating the separator copies the new first key and its CV into the parent, and further up while the node is a first child. | PASS |
| `/resolver/TempConflictBTree/PropagateUpdatesSeparatorOfInternalNode` | The same for an internal node's first separator; on the root, and on a root leaf, nothing changes. | PASS |
| `/resolver/TempConflictBTree/EraseAtKeepsLeaf` | Erasing entries that leave their leaf non-empty (middle; first, with the separators following across levels; last with a leaf to the right; last of the last leaf): the cursor identifies the next entry or the end position, and the tree matches the model. | PASS |
| `/resolver/TempConflictBTree/EraseAtPropagatesSplitCleared` | A leaf that stops needing a split by an erasure is no longer counted, through a parent with one free slot. | PASS |
| `/resolver/TempConflictBTree/EraseAtKeepsSplitOfFullLeaf` | A full leaf still needs a split after losing one entry, and the counts above do not change. | PASS |
| `/resolver/TempConflictBTree/EraseAtRemovesLeaf` | Erasing the only entry of a leaf removes it: neighbours linked, parent count reduced, cursor at the next leaf or at the end of the previous one, separators above following the parent's new first key. | PASS |
| `/resolver/TempConflictBTree/EraseAtRemovesEmptyParents` | Removing the last leaf of an internal node removes the node, and a root left with one child is replaced by it, down to a leaf, which becomes the only node in the age list; erasing its last entry empties the tree. | PASS |
| `/resolver/TempConflictBTree/EraseAtRemovesChainToRoot` | The only leaf left, under a chain of internal nodes with one child each, is erased with all of them. | PASS |
| `/resolver/TempConflictBTree/DetachLeaf` | Detaching a leaf in the middle of its parent returns it without parent and with no outer links, and links its neighbours. | PASS |
| `/resolver/TempConflictBTree/DetachInternalNode` | Detaching an internal node takes its whole run of leaves out of the chain, still linked among themselves. | PASS |
| `/resolver/TempConflictBTree/DetachFirstChildUpdatesSeparators` | Detaching a first child changes the separators above while the node is itself a first child. | PASS |
| `/resolver/TempConflictBTree/DetachUpdatesSplitCounts` | The parent no longer counts a detached child that needed a split, and a parent that needed one stops when it gets a slot back. | PASS |
| `/resolver/TempConflictBTree/DetachRemovesEmptyParents` | Detaching the last child of an internal node removes the node, and a root left with one child is replaced by it. | PASS |
| `/resolver/TempConflictBTree/DetachEmptiesTree` | Detaching the only leaf, under a chain of internal nodes with one child each, empties the tree. | PASS |

Writes. Except in `AddWritesAgainstReference`, each write is made with
`addWrite` and checked against a model that replaces what the write covers
and keeps the parts of ranges outside it with their CVs; after the write,
the tree check runs and its entries must equal the model.

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/AddWriteKinds` | Fifteen writes, each on a fresh tree of three leaves of three ranges: inside a range, over the start or end of ranges, across leaves, with boundaries equal to those of ranges, exactly a gap, inside a gap, before, after and over every range. | PASS |
| `/resolver/TempConflictBTree/AddWriteAgainstModel` | 2,000 writes at random places and of random lengths, from an empty tree with a 4 MiB budget: the structure matches the model after each one and the tree check runs every 250 writes and at the end; the tree reaches three levels. | PASS |
| `/resolver/TempConflictBTree/AddWriteAcrossLeaves` | Two writes from the first leaf into and past the second keep the separators in order. | PASS |
| `/resolver/TempConflictBTree/AddWriteErasesWholeTree` | A write covering a tree of nine leaves erases every node; its range is alone in a new root leaf, the only node in the age list; a further write works on it. | PASS |
| `/resolver/TempConflictBTree/AddWriteToEmptyTree` | A write into an empty tree creates the root leaf; a second, overlapping write keeps the left exterior. | PASS |
| `/resolver/TempConflictBTree/AddWriteTrimsFirstRangeOfLeaf` | A write from past the end of a leaf into the first range of the next goes after the last entry of the leaf `findKey` reached; the trimmed range keeps its CV, and the separators of its leaf and parent become its new start, with that CV. | PASS |
| `/resolver/TempConflictBTree/PreserveRightExteriorUpdatesSeparator` | Trimming the first range of a leaf moves the separators to the new start, keeping the CV, up while the leaf is a first child; trimming another range changes no separator; an end at or beyond the range's bounds, a null leaf or the end position change nothing. | PASS |
| `/resolver/TempConflictBTree/AddWriteAtEndOfLeafBeforeFullLeaf` | Writes past the last entry of a leaf, before a full leaf, go into the leaf `findKey` reached, which splits; the full leaf keeps its 59 entries. | PASS |
| `/resolver/TempConflictBTree/AddWriteAtFoundKey` | Writes starting at the begin of a range, including the first entry of a leaf, keep its right exterior or remove it when covered. | PASS |
| `/resolver/TempConflictBTree/AddWriteInsideRangeOfPreviousLeaf` | When the previous range is in the leaf before and reaches past the separator of the leaf reached, it is trimmed in place and the write, with its right exterior when it lies inside the range, goes first in the leaf reached, whose separator becomes the write's begin. | PASS |
| `/resolver/TempConflictBTree/AddWriteRemovesSubtreeFromEndOfLeaf` | A write from the end of a leaf across a whole internal node removes its leaves and the node; the write stays in the leaf it started from, and the age list holds exactly the nodes left. | PASS |
| `/resolver/TempConflictBTree/AddWriteInsideRangeTakesTwoSlots` | A write inside a range of a leaf with exactly two free slots takes both; the next such write finds the leaf full and splits it first. | PASS |
| `/resolver/TempConflictBTree/ScatteredPointsRegression` | 2,500 batches of scattered point reads and point writes over 100,000 keys, with a 16 MiB budget: every read within the last 1,000 versions agrees with a model of the points through `hasConflict`, the structure is checked every 500 batches, and the entries equal the model at the end. | PASS |
| `/resolver/TempConflictBTree/AddWritesAgainstReference` | 60 calls of `addWrites`, each with up to 12 sorted, disjoint intervals over keys 0 to 239, some adjacent, at increasing CVs, with a 4 MiB budget and no reclamation; after each call, 40 reporting single-read transactions are checked with `checkReads`, and each status and conflict bit equals a brute-force scan of every write made; from the sixth call on, some transactions conflict and some do not. | PASS |

Reclamation, reclaimed keys and room for writes. Some tests place two dead
entries whose keys fill two key blocks between live ones, reclaim those
blocks and overwrite every free block, so that a stale key reference
cannot keep working by accident.

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/HasConflictSkipsReclaimedKeysToTheRight` | A read running into a leaf with dead entries crosses them without reading their keys, and conflicts only with live entries. | PASS |
| `/resolver/TempConflictBTree/HasConflictSkipsReclaimedPreviousKey` | A read whose previous entry is dead does not read its key. | PASS |
| `/resolver/TempConflictBTree/AddWriteOverReclaimedKeys` | A write across dead entries erases them unread. | PASS |
| `/resolver/TempConflictBTree/AddWriteAfterReclaimedPreviousKey` | A write whose previous entry is dead erases it unread and goes on from there. | PASS |
| `/resolver/TempConflictBTree/ReclaimCVNeverDecreases` | When a later key block holds only older records, reclaiming it does not move `reclaimCV` back, and entries whose keys are gone stay expired. | PASS |
| `/resolver/TempConflictBTree/RemoveSubtreeOfRootLeaf` | Removing the root leaf empties the tree and the age list and returns its block. | PASS |
| `/resolver/TempConflictBTree/RemoveSubtreeOfWholeTree` | Removing the root of a tree of 13 nodes returns every block. | PASS |
| `/resolver/TempConflictBTree/RemoveSubtreeAtEachPosition` | Leaves and internal nodes at the first, middle and last rank of parents whose order is not the identity are removed with their subtrees; the age list holds exactly the remaining nodes and the freed blocks return to the pool. | PASS |
| `/resolver/TempConflictBTree/RemoveSubtreeCollapsesRoot` | Removing the last child of a node removes the node too, and the root left with one child is replaced by it. | PASS |
| `/resolver/TempConflictBTree/RemoveSubtreeUpdatesSplitCounts` | A removed child that needed a split is no longer counted above. | PASS |
| `/resolver/TempConflictBTree/ReclaimOldestWithNothingToReclaim` | With nothing stored, `reclaimOldest` returns false and changes nothing. | PASS |
| `/resolver/TempConflictBTree/ReclaimOldestOnlyKeys` | With only key blocks, the oldest is reclaimed and `reclaimCV` becomes its CV; the empty open block with CV 0 is reclaimed next without moving the cut back. | PASS |
| `/resolver/TempConflictBTree/ReclaimOldestOnlyNodes` | With only an empty root leaf with CV 0, it is reclaimed and the tree is empty. | PASS |
| `/resolver/TempConflictBTree/ReclaimOldestKeysFirst` | When the oldest key block is older than every node, only it is reclaimed. | PASS |
| `/resolver/TempConflictBTree/ReclaimOldestNodesFirst` | When the oldest node is older than the oldest key block, only it is reclaimed. | PASS |
| `/resolver/TempConflictBTree/ReclaimOldestTiesAndEqualAges` | A tie between the stores and several nodes of the same age: one call reclaims all of them, and the root left with one child is replaced. | PASS |
| `/resolver/TempConflictBTree/ReclaimOldestOneAgeAtATime` | Each call reclaims one age only, and the cut only moves forward. | PASS |
| `/resolver/TempConflictBTree/ReclaimOldestWaitsForConservativeCV` | A leaf whose entries are older than its maximum CV keeps them, expired, until the cut reaches that CV. | PASS |
| `/resolver/TempConflictBTree/ReclaimUntilAvailCases` | Nothing is reclaimed when enough blocks are free or none are asked for; one or two ages are reclaimed as needed; one age may free more than asked; asking for more than everything reclaims all and returns false. | PASS |
| `/resolver/TempConflictBTree/ReclaimAndWriteAgainstModel` | 1,500 operations in a 256 KiB pool, every seventh an explicit reclamation and the others writes with keys of 3 to 302 bytes: after each one the structure holds the live entries of the model and the age list holds exactly the tree's nodes; every 50 operations free blocks are overwritten and the tree check runs. | PASS |
| `/resolver/TempConflictBTree/AddWritesReclaimsRoomForLongKeys` | In a pool of 9 pages filled with point writes until history is reclaimed, a range of 30,001-byte keys, then a write of 30,001-byte keys inside it, which needs 92 blocks for itself and its two exteriors, more than the 35 reserved for short keys: `addWrites` reclaims room for all of it, the cut stays below the range's CV, and `checkReads` finds the left exterior, the write and the right exterior with their CVs. | PASS |
| `/resolver/TempConflictBTree/AddWritesThrowsWhenThePoolCannotHoldAWrite` | In a pool of 64 blocks, which holds the reserve for short keys but not the 197 blocks of one write of 65,000-byte keys: that write throws `std::length_error` after reclaiming everything (the cut reaches the CV of the earlier write), and the tree then takes an ordinary write, which `checkReads` finds. | PASS |
| `/resolver/TempConflictBTree/AddWritesKeepsTheReserveOfALongKeyAfterReclaimingIt` | In a pool of 12 pages (384 blocks), one range of 30,001-byte keys and then 20,000 point writes of distinct short keys, against the same point writes alone: the long range is reclaimed, yet the first tree keeps the 92-block reserve and ends with a higher `reclaimCV` (12,413 against 10,905); reads at each tree's `reclaimCV` of 207 written keys, one in 97, and of the six around the cut, give the outcomes the write CVs determine, none too old. | PASS |

Batch read check. Unless stated otherwise, the batch is checked with
G = 16 and against the batch reference.

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/CheckReadsInEmptyTree` | In an empty tree nothing conflicts. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWithoutReads` | A batch with only write ranges leaves every status unchanged, including one already set, and has no conflict bits. | PASS |
| `/resolver/TempConflictBTree/CheckReadsAroundOneRange` | The reads of `HasConflictAroundOneRange`, one per transaction, with and without reporting, give the same outcomes. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWithPoint` | A stored point conflicts only with reads containing its key. | PASS |
| `/resolver/TempConflictBTree/CheckReadsSkipsEmptyReads` | Reads with begin equal to or after end check nothing, even over a newer range. | PASS |
| `/resolver/TempConflictBTree/CheckReadsSkipsConflictingTransactions` | A transaction already marked is not checked again unless it reports, and its flag is never cleared. | PASS |
| `/resolver/TempConflictBTree/CheckReadsReportsEveryConflictingRead` | A reporting transaction has each conflicting read marked; a non-reporting one is marked with at least one bit. | PASS |
| `/resolver/TempConflictBTree/CheckReadsDuplicateReads` | The same read in several transactions, and twice in one, has the same outcome everywhere. | PASS |
| `/resolver/TempConflictBTree/CheckReadsLongReadAcrossLeaves` | A read spanning eight leaves conflicts only through the last range of the last leaf, at versions below its CV. | PASS |
| `/resolver/TempConflictBTree/CheckReadsMarksTooOldTransactions` | With `reclaimCV` 20, transactions with read versions 19 and 5 are marked too old and counted once each, including the one with two reads, and none of their reads has a bit; a transaction at version 20 is checked. | PASS |
| `/resolver/TempConflictBTree/CheckReadsAgainstReference` | 300 leaves with random CVs and some single-entry leaves, under fanouts 2, 3 and 40, and four random batches of 1,500 transactions on each tree: G = 16 and G = 32 both agree with the reference and give the same statuses; some transactions conflict and some do not. With fanout 2 the partition creates more tasks than G, so tasks are suspended and resumed. | PASS |
| `/resolver/TempConflictBTree/CheckReadsSkipsExpiredEntriesAcrossLeaves` | Expired entries are not read and are removed from the leaves the batch visits; a read whose leaf holds only expired entries before its position finds the previous live entry in a leaf to its left, or none. | PASS |
| `/resolver/TempConflictBTree/CheckReadsRemovesLeavesLeftEmpty` | A leaf whose entries have all expired is emptied by the batch and removed after it; the queue is empty afterwards and no outcome changes. | PASS |
| `/resolver/TempConflictBTree/CheckReadsRoutesPastEmptyChildMarks` | A separator marked `EmptyChildCV` sends reads to the left, and the leaf chain still reaches the entries to the right, including for a read from the empty child's range into the next leaf. | PASS |
| `/resolver/TempConflictBTree/CheckReadsMatchesHasConflictOnWrittenTree` | 120 batches of up to 300 random transactions on a tree grown by writes (16 MiB budget), alternating G = 16 and G = 32: every read bit and status agrees with `hasConflict` read by read, computed before the batch; after each batch, up to 39 sorted, disjoint writes are added. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWithLongKeys` | Stored points and random reads built from keys of 153 bytes, longer than a cache line, with records spanning key blocks, 400 transactions: the result agrees with the reference; some transactions conflict and some do not, and among the non-empty reads some conflict and some do not. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWithEveryEntryExpired` | Three leaves under an internal root, every entry expired: two reads find no conflict, and the leaves left empty are removed after the batch, with the queue empty. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWithEveryEntryExpiredInRootLeaf` | A root leaf with every entry expired: the read finds no conflict, and the structure check passes after the batch. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWalksLeftPastExpiredLeaves` | Four leaves under the root, the middle two expired: a read in the first expired leaf finds no conflict, a read from the second expired leaf into the last leaf conflicts, and a read in the first leaf conflicts. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWalksLeftIntoPreviousSubtree` | Eight leaves under two internal nodes, the first three leaves of the second node expired: a read in its live leaf, routed into its first leaf, looks for the previous live entry in the first subtree; outcomes {0, 1, 0} as the reference. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWithEmptyKeys` | A stored range whose begin is the empty key, and reads whose begin is the empty key: outcomes {1, 0, 0} as the reference. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWithRangeEndInLaterKeyBlock` | A stored general range with a 2,100-byte begin, so that its 140-byte end starts in a later key block: a read beginning inside it conflicts and a read beginning past it does not. | PASS |
| `/resolver/TempConflictBTree/CheckReadsPastSeparatorsInReusedKeyBlock` | Nine leaves, three per internal node, with the subtree in the middle, and in a second tree the first subtree, expired; the separators leading to it, in its parent and in itself, point into the oldest key block, which is reclaimed and handed out again, so they point at other bytes. Reads from the expired subtree into live history, inside it, into it from before, elsewhere, and newer than every entry give outcomes {1, 0, 0, 1, 0} as the reference, and the queue is empty afterwards. | PASS |

125 TempConflictBTree tests passed, 0 failed. The full run, including the
15 PagePool tests, the 41 KeyStore tests and the 47 BNodeStore tests,
reports 228 passed and 0 failed.

The tree checks its internal invariants, the results of its block
allocations and the results of the key records a write stores with
`assert`, which terminates the process when it fails and is removed when
`NDEBUG` is defined. The tested builds do not define `NDEBUG`. Through the
interface these checks can fail only when a precondition does not hold;
inputs that do not meet the preconditions are outside the interface and
are not tested.

These preconditions are part of the interface but are not checked:

- The order of `points` and the batch layout described in
  [Interface contract](#interface-contract). `checkReads` does not verify
  them.
- `addWrites` requires sorted, disjoint ranges, which are not checked.
  A CV that decreases between calls is detected only indirectly, when a
  write allocates or splits a node: by the allocation check of
  `BNodeStore` or by the split check `cv >= node->maxCV`.
- In the `ConflictBatch` of this repository (`ConflictSet.cpp`) every read
  of a transaction carries the transaction's read snapshot. The interface
  allows a version per read; the tests use one version per transaction.

### Reserve after a long key

The reserve follows the longest key written so far, including the keys of
the incoming write, and it is not lowered when that key is reclaimed. The
policy is conservative: one long key keeps every later write reserving
room for it, so reclamation starts earlier for the rest of the tree's
life. The policy is not changed here; its effect was measured with
`tests/evidence/longkeyreserve.cpp`, which repeats the test above with
other key lengths and pools:

```bash
for b in 1024 2048 4096 8192; do
	clang++ -std=c++20 -O2 -DRESOLVER1_BLOCK_SIZE=$b -I. tests/evidence/longkeyreserve.cpp pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp -o build/lk$b
done
build/lk2048 30001 12   # key length, pool pages
```

One write of the given length, then 20,000 short point writes (CVs 2 to
20,001), against the same short writes alone. "Retained" is the number of
CVs above `reclaimCV`, out of 20,001.

| Block | Pool | Long key | Reserve | `reclaimCV` alone | `reclaimCV` after long key | Retained alone | Retained after long key |
|---|---|---|---|---|---|---|---|
| 1 KiB | 768 blocks | 10,001 | 65 | 11,897 | 12,235 | 8,104 | 7,766 |
| 1 KiB | 768 blocks | 30,001 | 185 | 11,897 | 13,574 | 8,104 | 6,427 |
| 2 KiB | 384 blocks | 10,001 | 35 | 10,905 | 10,905 | 9,096 | 9,096 |
| 2 KiB | 384 blocks | 30,001 | 92 | 10,905 | 12,413 | 9,096 | 7,588 |
| 4 KiB | 192 blocks | 10,001 | 35 | 11,401 | 11,401 | 8,600 | 8,600 |
| 4 KiB | 192 blocks | 30,001 | 47 | 11,401 | 12,181 | 8,600 | 7,820 |
| 8 KiB | 192 blocks | 10,001 | 35 | 2,043 | 2,196 | 17,958 | 17,805 |
| 8 KiB | 192 blocks | 30,001 | 35 | 2,043 | 2,196 | 17,958 | 17,805 |
| 8 KiB | 192 blocks | 60,000 | 47 | 2,043 | 3,538 | 17,958 | 16,463 |

With 64 pages (4 MiB) no run reclaimed anything, with or without the long
key, at any block size. With 2 KiB blocks and 30,001-byte keys, the
largest FDB's client sends, the reserve grows by 57 blocks: about 15% of
this 384-block pool, and about 0.01% of the 1 GiB pool of the FDB
integration. With 8 KiB blocks and keys of up to 30,001 bytes the reserve
stays at 35, yet `reclaimCV` still ends 153 CVs ahead after the long key;
that difference does not come from the reserve, and its cause was not
investigated. The runs drive the tree directly; the effect on a replayed
FDB workload was not measured.

## Coverage

Coverage was collected running only the TempConflictBTree tests.

gcov, `tempconflictbtree.cpp`:

| Metric | Result |
|---|---|
| Lines executed | 99.20% of 1128 |
| Branches executed | 100.00% of 826 |
| Taken at least once | 84.38% of 826 |

llvm-cov source-based coverage:

| File | Regions | Functions | Lines | Branches |
|---|---|---|---|---|
| `tempconflictbtree.cpp` | 99.17% of 719 | 100.00% of 71 | 99.01% of 1812 | 97.23% of 506 |
| `tempconflictbtree.h` | 91.67% of 12 | 88.89% of 9 | 52.17% of 46 | 100.00% of 2 |

These are gcov and llvm-cov metrics. They are not a measurement of
MC/DC, and full coverage would not show that the code is free of errors.
"Taken at least once" does not mean that every decision was taken in both
directions. Apple clang 15 provides no condition coverage equivalent to
GCC's `-fcondition-coverage`.

gcov counts branches between the basic blocks of the generated code,
including the exception edges the compiler adds and the failure paths of
the `assert` checks. llvm-cov counts the true and false outcomes of each
condition written in the source outside the `assert` checks, whose
conditions come from a macro in a system header.

`checkReads` is a template instantiated for G = 16 and G = 32, and its
lambdas are separate functions in each instantiation. gcov shows both
instantiations on the same source line, with the items of `checkReads<16>`
first. llvm-cov shows each instantiation separately; counting an outcome
as covered when either instantiation takes it gives the 253 conditions,
506 branches and 14 uncovered branches of its report.

The annotated gcov file marks 6 branch items as never executed and 123 as
taken 0 times: 129 items, which is 826 minus the 697 taken at least once.
The 6 never-executed items (items 2 and 3 of line 186, 6 and 7 of lines
1004 and 1176) are on lines that ran, which is why the summary reports
every branch as executed.

The lines llvm-cov reports as not executed in `tempconflictbtree.cpp` are
lines 601-604, 1226-1228 and 1302-1305, the `default` of `runRound`
(line 1520) and the closing braces that follow a `break` in its cases
(lines 1495-1519); see
[Uncovered generated branches](#uncovered-generated-branches). In
`tempconflictbtree.h`, the function not executed is the template
`makePrefetchOrder` (lines 55-76, the 22 lines not executed): it is
evaluated at compile time to initialize the constant `prefetchOrder`,
which `tempconflictbtree.cpp` does not reference. The other 8 functions of
the header (the destructor, `reclaimCV`, `unlinkLeaf`, the three
`needsSplit` overloads and two `propagateUpdates` overloads) are executed,
and the two outcomes of the condition of the header overload of
`needsSplit` (line 269) are covered.

`checkReads` also fills the member `writeBegins` from the write endpoints
of the batch; nothing reads it, and no result depends on it.

### Independent condition checks

`tempconflictbtree.cpp` has 35 compound decisions outside the `assert`
checks. For each, the table isolates every condition: the input makes that
condition decide while the conditions before it have the values given.
The test named is one that builds that configuration; the llvm-cov counts
that follow show which outcomes occurred, not which test produced them.

The tables assume the values of the tested platform and build:
`BlockSize` 2048, `sizeof(KeyBlockHeader)` 24 (a key-block payload of
2,024 bytes), `LeafCapacity` 59, `InternalCapacity` 40, 32 blocks per
64 KiB page, and `EmptyChildCV` equal to -1, below every `reclaimCV_`,
which starts at 0 and never decreases. The prefetch code uses a fixed line
size of 64 bytes, which is not read from the platform.

**Writes.**

`addWrite`: throw at `!reclaimUntilAvail(max(35, writeBlocks)) && availableBlocks < writeBlocks`
(lines 37-38).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Reserve reached | Any write in a pool that can free the reserve | Not evaluated | Store | `AddWritesReclaimsRoomForLongKeys` |
| Reserve not reached, the write fits | Pool of fewer than 35 blocks, short keys | Reserve not reached | Store | Not exercised |
| Reserve not reached, the write does not fit | 65,000-byte keys in a 64-block pool | Reserve not reached | Throw | `AddWritesThrowsWhenThePoolCannotHoldAWrite` |

The second condition is never false in the tests; llvm-cov reports that
outcome as not covered.

`insertWithLeftExterior`: `hasPrevious = !found && prevRank(...)` (line 83).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Key found | Write begins at the begin of a range | Not evaluated | No previous | `AddWriteAtFoundKey` |
| No previous entry | Write before every range | Not found | No previous | `AddWriteKinds` (90, 95) |
| Previous entry | Write after a range | Not found | Previous | `AddWriteKinds` (105, 115) |

`expired = hasPrevious && cv <= reclaimCV_` (line 85).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| No previous | As above | Not evaluated | Not expired | `AddWriteKinds` (90, 95) |
| Previous live | CV above the cut | Previous | Not expired | `AddWriteKinds` (105, 115) |
| Previous expired | CV at or below the cut | Previous | Expired | `AddWriteAfterReclaimedPreviousKey` |

`hasPrevious && !expired && compareEnd(write.begin, previous) < 0`, keep
the left exterior (lines 86-87).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| No previous | As above | Not evaluated | No trim | `AddWriteKinds` (90, 95) |
| Previous expired | As above | Previous | No trim, key not read | `AddWriteAfterReclaimedPreviousKey` |
| Previous ends at or before the write | Write inside a gap | Previous, live | No trim | `AddWriteKinds` (112, 118) |
| Previous overlaps the write's begin | Write over the end of a range | Previous, live | Trim | `AddWriteKinds` (105, 115) |

`eraseCoveredRanges`: stop at `cv > reclaimCV_ && compareEnd(end, entry) < 0`
(lines 130-131).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Entry expired | Dead entry under the write | Not evaluated | Erase, key not read | `AddWriteOverReclaimedKeys` |
| Live entry ends at or before the write's end | Covered range | Live | Erase | `AddWriteKinds` (105, 165) |
| Live entry ends after the write's end | Range over the write's end | Live | Stop | `AddWriteKinds` (105, 125) |

`preserveRightExterior`: return at `leaf == nullptr || rank >= count`
(line 142).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| No leaf | Null leaf | Not evaluated | Return | `PreserveRightExteriorUpdatesSeparator` |
| End position | Rank equal to the count | Leaf present | Return | `PreserveRightExteriorUpdatesSeparator` |
| Entry present | Rank below the count | Leaf present | Continue | `PreserveRightExteriorUpdatesSeparator` |

Return at `compareBegin(end, entry) <= 0 || compareEnd(end, entry) >= 0`
(lines 147-148).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| End at or before the entry's begin | `end` equal to the begin | Not evaluated | Return | `PreserveRightExteriorUpdatesSeparator` (285 on `[285, 290)`) |
| End at or past the entry's end | `end` equal to or beyond the end | Begin below `end` | Return | `PreserveRightExteriorUpdatesSeparator` (290, 400) |
| End strictly inside | `end` inside the entry | Begin below `end` | Trim | `PreserveRightExteriorUpdatesSeparator` (285 on `[280, 290)`) |

**Search and structure.**

`findKey`: step back to the previous child at `!found && rank > 0`
(line 195).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Key equals a separator | A leaf's lower bound | Not evaluated | Stay | `FindKeyThroughOneInternalLevel` |
| Key before the first separator | Rank 0 | Not found | Stay at child 0 | `FindKeyBeforeFirstSeparator` |
| Key between separators | Rank above 0 | Not found | Step back | `FindKeyThroughOneInternalLevel` |

`insertAt`: count a new split at `!splitBefore && splitAfter` (line 233).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Already needed a split | Leaf of 58 entries gets its 59th | Not evaluated | No change | `InsertAtPropagatesNeedsSplit` |
| Still has room | Leaf of 3 entries | Did not need | No change | `InsertAtUpdateCombinations` |
| Starts needing a split | Leaf of 57 entries gets its 58th | Did not need | +1 | `InsertAtPropagatesNeedsSplit` |

`normalizeRoot`: stop at `kind != Internal || count != 1` (line 243).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Root is a leaf | Root replaced down to a leaf | Not evaluated | Stop | `EraseAtRemovesEmptyParents` |
| Internal root with two children | Root replaced by an internal node with two leaves | Internal | Stop | `EraseAtRemovesEmptyParents` |
| Internal root with one child | One child left | Internal | Replace | `EraseAtRemovesEmptyParents` |

`eraseAt` on a leaf: uncount a split at `splitBefore && !needsSplit(leaf)`
(line 261).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Did not need a split | Leaf of three entries | Not evaluated | No change | `EraseAtKeepsLeaf` |
| Still needs one | 59 entries to 58 | Needed | No change | `EraseAtKeepsSplitOfFullLeaf` |
| Stops needing one | 58 entries to 57 | Needed | -1 | `EraseAtPropagatesSplitCleared` |

Update the separator at `rank == 0 && count > 0` (line 262).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Not the first entry | Middle entry erased | Not evaluated | No update | `EraseAtKeepsLeaf` |
| Leaf left empty | Only entry erased | First entry | No update | `EraseAtRemovesLeaf` |
| Leaf keeps entries | First of three erased | First entry | Update | `EraseAtKeepsLeaf` |

Removal of ancestors left empty, `count == 0 && parent() != nullptr`, in
`eraseAt` on a leaf (line 275), `eraseAt` on an internal node (line 323)
and `removeIfEmpty` (line 608).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Ancestor keeps children | Parent with another child | Not evaluated | Stop | `EraseAtRemovesEmptyParents`, `DetachRemovesEmptyParents`, `DeferredRefreshMarksEmptyInternalChild` |
| Empty ancestor is the root | Chain of single children up to the root | Empty | Stop; `normalizeRoot` releases the root | `EraseAtRemovesChainToRoot`, `DetachEmptiesTree`, `FirstLiveKey` |
| Empty ancestor with a parent | Last child of an internal node removed | Empty | Remove it and go up | `EraseAtRemovesEmptyParents`, `DetachRemovesEmptyParents`, `DeferredRefreshMarksEmptyInternalChild` |

`reclaimOldest`: nothing to reclaim at `!keyCV && !nodeCV` (line 386).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Key blocks present | Only key blocks | Not evaluated | Reclaim | `ReclaimOldestOnlyKeys` |
| Nodes present | Only nodes | No key blocks | Reclaim | `ReclaimOldestOnlyNodes` |
| Nothing stored | Empty stores | No key blocks | Return false | `ReclaimOldestWithNothingToReclaim` |

`refreshSeparator`: refresh the first grandchild while
`count > 0 && first separator expired` (lines 423-424).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Child left empty | Every grandchild expired | Not evaluated | Stop; remove the child | `RefreshSeparatorRemovesEmptyInternalChild` |
| First separator live | Third grandchild live | Child not empty | Stop | `RefreshSeparatorSkipsExpiredGrandchildren` |
| First separator expired | First grandchild expired | Child not empty | Refresh it | `RefreshSeparatorSkipsExpiredGrandchildren` |

`cleanExpired` (line 567) and `removeEmptyChild` (line 590): uncount a
split at `splitBefore && !needsSplit(node)`.

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Leaf did not need a split | 57 entries, one removed | Not evaluated | No change | `CleanExpiredReportsLeafTransitionOnce` |
| Leaf still needs one | 59 entries, one removed | Needed | No change | `CleanExpiredReportsLeafTransitionOnce` |
| Leaf stops needing one | 59 entries, two removed | Needed | -1 | `CleanExpiredReportsLeafTransitionOnce` |
| Parent did not need a split | Parent with free slots | Not evaluated | No change | `RefreshSeparatorRemovesEmptyLeaf` |
| Parent stops needing one | Parent with one free slot loses an empty child | Needed | -1 | `RemoveEmptyChildPropagatesSplitCleared` |

In `removeEmptyChild` the second condition is never false when the first
is true: the empty child is not counted as needing a split, so erasing it
adds a free slot and leaves the count unchanged, and a node whose count
equalled its free slots no longer does. llvm-cov reports this outcome as
not covered.

`splitLeaf`: the position moves to the new leaf at
`rank > cutRank || (found && rank == cutRank)` (line 716).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Beyond the cut | Key after the cut entry | Not evaluated | New leaf | `SplitRootLeaf` |
| Before the cut | Key before the cut entry | Not beyond | Old leaf | `SplitRootLeaf` |
| Found at the cut | The cut entry's begin | Not beyond, found | New leaf | `SplitRootLeaf` |
| Not found at the cut | Key just before the cut entry | Not beyond, not found | Old leaf, at its end | `SplitRootLeaf` |

`propagateUpdates`: continue while
`parent != nullptr && (splitDelta != 0 || updateSeparator || updateCV)`
(line 784).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Root reached | Update reaching the root | Not evaluated | Stop | `InsertAtPropagatesMaxCV` (first entry) |
| Only the split state changes | Leaf starts needing a split | Parent present | Continue | `InsertAtPropagatesNeedsSplit` |
| Only the separator changes | First entry removed | Parent present | Continue | `PropagateUpdatesSeparator` |
| Only the CV changes | Newer entry | Parent present | Continue | `InsertAtPropagatesMaxCV` |
| Nothing left to update | Parent's CV already as high | Parent present | Stop | `InsertAtPropagatesMaxCV` (second entry) |

**Single-read check.**

`hasConflict`: conflict with the previous entry at
`slotPresent && cv > read.version && compareEnd(read.begin, previous) < 0`
(lines 832-833).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| No previous entry | Read below every remaining key | Not evaluated | No conflict here | `HasConflictAfterFirstLeafRemoved` |
| Previous not newer | `[k(25), k(26))` at version 10 against CV 10 | Previous present | No conflict here | `HasConflictAroundOneRange` |
| Previous ends at the read's begin | `[k(30), k(40))` at version 0 | Previous present and newer | No conflict here | `HasConflictAroundOneRange` |
| Previous overlaps the read | `[k(25), k(26))` at version 9 | Previous present and newer | Conflict | `HasConflictAroundOneRange` |

**Batch read check.** Counts below are per instantiation, G = 16 / G = 32.

`buildBeginIndexes`: skip a read at
`transactionConflictStatus[t] && conflictingKeyRange == nullptr`
(lines 1004-1005).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Transaction not marked | Fresh transaction | Not evaluated | Keep | `CheckReadsSkipsConflictingTransactions` |
| Marked, reporting | Already conflicting, reporting | Marked | Keep | `CheckReadsSkipsConflictingTransactions` |
| Marked, not reporting | Already conflicting, not reporting | Marked | Skip | `CheckReadsSkipsConflictingTransactions` |

`readStillNeeded`: not needed at `tooOld || conflicts[rangeIndex]`
(lines 1031-1032), then needed at
`!transactionConflictStatus[t] || conflictingKeyRange != nullptr`
(lines 1033-1034).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Transaction too old | Not reachable; see below | Not evaluated | Not needed | - |
| Read already marked | Read marked against its previous entry, met again in the scan | Not too old | Not needed | `CheckReadsAroundOneRange` (reporting) |
| Transaction not marked | Fresh transaction | Read not marked | Needed | `CheckReadsAroundOneRange` |
| Marked, reporting | Another read of it conflicted | Read not marked | Needed | `CheckReadsReportsEveryConflictingRead` |
| Marked, not reporting | Another read of it conflicted | Read not marked | Not needed | `CheckReadsReportsEveryConflictingRead` |

`buildBeginIndexes` removes the reads of every too-old transaction, and
no transaction becomes too old during the traversal, so `tooOld` is never
true there. llvm-cov reports that outcome as not covered.

`prefetchStoredKeySlow`: return at `length == 0 || !isValid(range)`
(line 1086). Neither condition is ever true; see
[Uncovered generated branches](#uncovered-generated-branches). The
decision has no outcome in which it returns, so no condition is isolated.
`CheckReadsPastSeparatorsInReusedKeyBlock` places expired separators in a
key block that has been reclaimed and handed out again, and the batch
completes with the reference outcomes.

Return at `length == 0 || line == 1` (line 1104).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Key ends within the first line | Short rest of key | Not evaluated | Return | `CheckReadsWithLongKeys` |
| Second line done | Key longer than the first line | Bytes left | Return | `CheckReadsWithLongKeys` |
| Bytes left after the first line | Key crossing a line | Bytes left, first line | Next line | `CheckReadsWithLongKeys` |

`toCompareKeys`: refresh at `separatorCV <= reclaimCV_ && separatorCV != EmptyChildCV`
(lines 1176-1177).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Separator live | CV above the cut | Not evaluated | No refresh | `CheckReadsAgainstReference` |
| Separator marked | `EmptyChildCV` | At or below the cut | No refresh | `CheckReadsRoutesPastEmptyChildMarks` |
| Separator expired | CV at or below the cut | Not marked | Refresh | `CheckReadsRemovesLeavesLeftEmpty` |

Skip reads no longer needed while `readIndex < readEnd && !readStillNeeded`,
in `checkPrev` (lines 1396-1397) and `startScan` (lines 1473-1474).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Slice exhausted | Last read of the task | Not evaluated | Stop | `CheckReadsAgainstReference` |
| Read needed | Next read still open | Reads left | Stop at it | `CheckReadsAgainstReference` |
| Read not needed | Next read of a non-reporting transaction already marked | Reads left | Skip it | `CheckReadsDuplicateReads` |

`endRound`: return at `!completedTasks && newTasks.empty()` (line 1528);
free a slot at `step == Empty || step == Done` (line 1538); insert a
sibling after its parent at
`created < newTasks.size() && newTasks[created].parent == t` (line 1544);
resume suspended tasks while `active < G && !pending.empty()` (line 1580).
These depend on how a batch's reads partition among children, and are
exercised by the large random batches.

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| A task finished (1528) | Task done this round | Not evaluated | Rebuild the order | `CheckReadsAgainstReference` |
| A sibling was created (1528) | Partition split the reads | No task finished | Rebuild the order | `CheckReadsAgainstReference` |
| Neither (1528) | Tasks only advanced | No task finished | Return | `CheckReadsAgainstReference` |
| Task empty (1538) | Not reachable; see below | Not evaluated | Free | - |
| Task done (1538) | Scan finished | Not empty | Free | `CheckReadsAgainstReference` |
| Task running (1538) | Task in another step | Not empty | Keep | `CheckReadsAgainstReference` |
| No sibling left (1544) | All siblings placed | Not evaluated | No insertion | `CheckReadsAgainstReference` |
| Sibling of another task (1544) | Next sibling's parent later in the order | Siblings left | No insertion | `CheckReadsAgainstReference` |
| Sibling of this task (1544) | Next sibling's parent at this position | Siblings left | Insert | `CheckReadsAgainstReference` |
| G tasks active (1580) | Order full | Not evaluated | Stop | `CheckReadsAgainstReference` (fanout 2) |
| Nothing suspended (1580) | Empty pending list | Below G | Stop | `CheckReadsAgainstReference` |
| Suspended task waiting (1580) | Pending list not empty | Below G | Resume it | `CheckReadsAgainstReference` (fanout 2) |

A task is set to `Empty` only by `inspectNode` on an internal node without
children (line 1302), which the tree's invariant excludes (see below), so
line 1538 never frees an empty task. llvm-cov reports that outcome as not
covered.

`checkReads`: return at `root_ == nullptr || readBegins.empty()`
(line 1594).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Empty tree | No root | Not evaluated | Return | `CheckReadsInEmptyTree` |
| No reads | Only write ranges | Root present | Return | `CheckReadsWithoutReads` |
| Tree and reads | Any other batch | Root present | Traverse | `CheckReadsAroundOneRange` |

llvm-cov counts for the conditions of the compound decisions. Conditions
in `checkReads` and its lambdas show G = 16 / G = 32.

| Location | Condition | True | False |
|---|---|---|---|
| `tempconflictbtree.cpp:37` | Reserve not reached | 1 | 221k |
| `tempconflictbtree.cpp:38` | Write does not fit | 1 | 0 |
| `tempconflictbtree.cpp:83` | `!found` | 45.2k | 175k |
| `tempconflictbtree.cpp:83` | `prevRank(...)` | 44.9k | 298 |
| `tempconflictbtree.cpp:85` | `hasPrevious` | 44.9k | 175k |
| `tempconflictbtree.cpp:85` | Previous expired | 1 | 44.9k |
| `tempconflictbtree.cpp:86` | `hasPrevious` | 44.9k | 175k |
| `tempconflictbtree.cpp:86` | `!expired` | 44.9k | 1 |
| `tempconflictbtree.cpp:87` | Previous overlaps the write's begin | 2.63k | 42.2k |
| `tempconflictbtree.cpp:130` | Entry live | 397k | 2 |
| `tempconflictbtree.cpp:131` | Entry ends after the write's end | 217k | 179k |
| `tempconflictbtree.cpp:142` | `leaf == nullptr` | 1 | 218k |
| `tempconflictbtree.cpp:142` | `rank >= count` | 673 | 217k |
| `tempconflictbtree.cpp:147` | End at or before the entry's begin | 216k | 1.16k |
| `tempconflictbtree.cpp:148` | End at or past the entry's end | 2 | 1.16k |
| `tempconflictbtree.cpp:195` | `!found` | 2.03M | 10.7k |
| `tempconflictbtree.cpp:195` | `rank > 0` | 2.01M | 10.4k |
| `tempconflictbtree.cpp:233` | `!splitBefore` | 222k | 20 |
| `tempconflictbtree.cpp:233` | `splitAfter` | 1.36k | 221k |
| `tempconflictbtree.cpp:243` | Root not internal | 10 | 87 |
| `tempconflictbtree.cpp:243` | Root count not 1 | 71 | 16 |
| `tempconflictbtree.cpp:261` | `splitBefore` | 296 | 178k |
| `tempconflictbtree.cpp:261` | `!needsSplit(leaf)` | 295 | 1 |
| `tempconflictbtree.cpp:262` | `rank == 0` | 812 | 178k |
| `tempconflictbtree.cpp:262` | `count > 0` | 787 | 25 |
| `tempconflictbtree.cpp:275` | Ancestor empty | 6 | 23 |
| `tempconflictbtree.cpp:275` | Ancestor has a parent | 5 | 1 |
| `tempconflictbtree.cpp:323` | Node empty | 4 | 29 |
| `tempconflictbtree.cpp:323` | Node has a parent | 3 | 1 |
| `tempconflictbtree.cpp:386` | `!keyCV` | 7 | 329 |
| `tempconflictbtree.cpp:386` | `!nodeCV` | 6 | 1 |
| `tempconflictbtree.cpp:423` | Child not empty | 49 | 5 |
| `tempconflictbtree.cpp:424` | First separator expired | 48 | 1 |
| `tempconflictbtree.cpp:567` | `splitBefore` | 39.3k | 1.45M |
| `tempconflictbtree.cpp:567` | `!needsSplit(leaf)` | 8 | 39.3k |
| `tempconflictbtree.cpp:590` | `splitBefore` | 2 | 132 |
| `tempconflictbtree.cpp:590` | `!needsSplit(parent)` | 2 | 0 |
| `tempconflictbtree.cpp:608` | Parent empty | 4 | 30 |
| `tempconflictbtree.cpp:608` | Parent has a parent | 3 | 1 |
| `tempconflictbtree.cpp:716` | `rank > cutRank` | 708 | 699 |
| `tempconflictbtree.cpp:716` | `found` | 193 | 506 |
| `tempconflictbtree.cpp:716` | `rank == cutRank` | 6 | 187 |
| `tempconflictbtree.cpp:784` | `parent != nullptr` | 451k | 203k |
| `tempconflictbtree.cpp:784` | `splitDelta != 0` | 1.71k | 450k |
| `tempconflictbtree.cpp:784` | `updateSeparator` | 6.56k | 443k |
| `tempconflictbtree.cpp:784` | `updateCV` | 243k | 199k |
| `tempconflictbtree.cpp:832` | `slotPresent` | 1.18M | 55.1k |
| `tempconflictbtree.cpp:832` | Previous newer than the read | 718k | 462k |
| `tempconflictbtree.cpp:833` | Previous overlaps the read | 29.8k | 688k |
| `tempconflictbtree.cpp:1004` | Transaction marked | 6 / 0 | 68.1k / 69.4k |
| `tempconflictbtree.cpp:1005` | Not reporting | 3 / 0 | 3 / 0 |
| `tempconflictbtree.cpp:1031` | `tooOld` | 0 / 0 | 307k / 348k |
| `tempconflictbtree.cpp:1032` | Read already marked | 217 / 159 | 307k / 348k |
| `tempconflictbtree.cpp:1033` | Transaction not marked | 282k / 321k | 24.6k / 26.7k |
| `tempconflictbtree.cpp:1034` | Reporting | 14.1k / 15.9k | 10.4k / 10.8k |
| `tempconflictbtree.cpp:1086` | `length == 0` | 0 / 0 | 18 / 13 |
| `tempconflictbtree.cpp:1086` | `!isValid(range)` | 0 / 0 | 18 / 13 |
| `tempconflictbtree.cpp:1104` | `length == 0` | 13 / 13 | 19 / 9 |
| `tempconflictbtree.cpp:1104` | `line == 1` | 5 / 0 | 14 / 9 |
| `tempconflictbtree.cpp:1176` | Separator at or below the cut | 32 / 0 | 26.3k / 26.0k |
| `tempconflictbtree.cpp:1177` | Separator not marked | 26 / 0 | 6 / 0 |
| `tempconflictbtree.cpp:1396` | `readIndex < readEnd` | 1.47k / 1.56k | 85 / 55 |
| `tempconflictbtree.cpp:1397` | `!readStillNeeded` | 202 / 224 | 1.27k / 1.34k |
| `tempconflictbtree.cpp:1473` | `readIndex < readEnd` | 5.57k / 5.40k | 17 / 10 |
| `tempconflictbtree.cpp:1474` | `!readStillNeeded` | 1.01k / 947 | 4.56k / 4.45k |
| `tempconflictbtree.cpp:1528` | `!completedTasks` | 5.79k / 4.81k | 2.99k / 2.15k |
| `tempconflictbtree.cpp:1528` | `newTasks.empty()` | 4.15k / 3.83k | 1.63k / 983 |
| `tempconflictbtree.cpp:1538` | `step == Empty` | 0 / 0 | 60.1k / 65.7k |
| `tempconflictbtree.cpp:1538` | `step == Done` | 4.57k / 4.46k | 55.5k / 61.2k |
| `tempconflictbtree.cpp:1544` | Siblings left | 28.2k / 36.8k | 31.9k / 28.8k |
| `tempconflictbtree.cpp:1544` | Sibling of this task | 4.42k / 4.39k | 23.8k / 32.4k |
| `tempconflictbtree.cpp:1580` | `active < G` | 3.69k / 3.30k | 3.20k / 1.60k |
| `tempconflictbtree.cpp:1580` | `!pending.empty()` | 2.26k / 1.77k | 1.42k / 1.52k |
| `tempconflictbtree.cpp:1594` | `root_ == nullptr` | 2 / 0 | 155 / 72 |
| `tempconflictbtree.cpp:1594` | `readBegins.empty()` | 1 / 0 | 154 / 72 |

The other decisions of `tempconflictbtree.cpp` have a single condition.
llvm-cov reports both outcomes covered for each of them, except those
listed in the next section.

### Uncovered generated branches

gcov reports 129 untaken branch items: 6 never executed and 123 taken 0
times. The origin of each was checked in the LLVM IR that Apple clang 15
generates for `tempconflictbtree.cpp` at `-O0` with `--coverage` and `-g`,
and against the llvm-cov branch counts. Line numbers refer to
`tempconflictbtree.cpp`; item numbers are those of the annotated gcov
file, where on the lines of `checkReads` and its lambdas the first half of
the items belongs to G = 16 and the second half to G = 32.

**Precondition and invariant checks (44 items).** The paths to
`__assert_rtn` taken when an `assert` fails. The IR contains a call to
`__assert_rtn` on each of these lines.

| Location | gcov items | Check |
|---|---|---|
| `tempconflictbtree.cpp:50` | 1 | Range of the write stored |
| `tempconflictbtree.cpp:79` | 1 | Two free slots for a write |
| `tempconflictbtree.cpp:93` | 1 | Left exterior stored |
| `tempconflictbtree.cpp:101` | 0 | Right exterior of a write inside a range stored |
| `tempconflictbtree.cpp:126` | 0 | Non-null leaf |
| `tempconflictbtree.cpp:152` | 0 | Right exterior stored |
| `tempconflictbtree.cpp:161` | 1 | Empty tree before creating the root leaf |
| `tempconflictbtree.cpp:163` | 0 | Root leaf allocated |
| `tempconflictbtree.cpp:186` | 0; 2 and 3 never executed | Only the root left without children |
| `tempconflictbtree.cpp:292` | 0 | Tree empty after removing the last leaf |
| `tempconflictbtree.cpp:320` | 0 | Node no longer needs a split after a detach |
| `tempconflictbtree.cpp:577` | 0 | Removed child is empty |
| `tempconflictbtree.cpp:625`, `626`, `627` | 1 each | Split of a non-null node that needs it, with a CV not below its maximum |
| `tempconflictbtree.cpp:633` | 0 | A split leaf is the current leaf |
| `tempconflictbtree.cpp:645` | 1 | Internal node allocated |
| `tempconflictbtree.cpp:657`, `658` | 1, 0 | Internal cut inside the node |
| `tempconflictbtree.cpp:691` | 1 | Leaf allocated |
| `tempconflictbtree.cpp:695`, `696` | 1, 0 | Leaf cut inside the leaf |
| `tempconflictbtree.cpp:725` | 0 | Non-empty node for its first key |
| `tempconflictbtree.cpp:741`, `742` | 1 each | Neither half needs a split |
| `tempconflictbtree.cpp:747` | 1 | New root allocated |
| `tempconflictbtree.cpp:759`, `760`, `764` | 1 each | Parent has a free slot and a counted child, and keeps its split state |
| `tempconflictbtree.cpp:773` | 0, 2 | Split delta between -1 and 1 |
| `tempconflictbtree.cpp:790` | 3 | A count is only lowered when positive |
| `tempconflictbtree.cpp:1040`, `1042` | 1 and 3; 0 and 2 | Removed read inside the task's slice |
| `tempconflictbtree.cpp:1549` | 0, 2 | Every new task placed |
| `tempconflictbtree.cpp:1570`, `1581` | 1 and 3 each | A free task slot when admitting or resuming a task |

On line 186, items 2 and 3 are the normal and exception edges of the call
to `__assert_rtn`, which is an `invoke` because `findKey` has a local
`std::vector` to destroy. The checks of the key records a write stores
(lines 50, 93, 101 and 152) and of node allocations (lines 163, 645, 691
and 747) fail only when the pool runs out of blocks during a write, which
the room `addWrite` reserves before the write excludes.

**Exception paths not exercised (35 items).** Exception edges of calls
(`invoke` instructions) in functions that have objects to destroy: the
constructor (its members), `addWrite` (the exception object being
constructed), `findKey` (a local `std::vector`) and `checkReads` (local
`std::vector`s). The IR contains an `invoke` on each line.

| Location | gcov items | Call |
|---|---|---|
| `tempconflictbtree.cpp:39` | 1 | `std::length_error` constructor, which copies its message |
| `tempconflictbtree.cpp:192` | 1 | `std::vector::push_back` |
| `tempconflictbtree.cpp:971` | 1, 3 | `std::vector::reserve` |
| `tempconflictbtree.cpp:973` | 1, 3 | `std::vector::assign` |
| `tempconflictbtree.cpp:976`, `977` | 1 and 3 each | `std::vector::reserve` |
| `tempconflictbtree.cpp:1593` | 1, 3 | `buildBeginIndexes`, which calls `push_back` |
| `tempconflictbtree.cpp:1595` | 1, 3 | `std::vector::resize` |
| `tempconflictbtree.cpp:1600` | 1, 3 | `runRound`, which may add to `pending_` |
| `tempconflictbtree.cpp:1601` | 1, 3 | `endRound`, which may add to the list of suspended tasks |
| `tempconflictbtree.cpp:8` | 1 | `KeyStore` constructor |
| `tempconflictbtree.cpp:9` | 0 | `BNodeStore` constructor |
| `tempconflictbtree.cpp:184` | 1 | `refreshSeparators` |
| `tempconflictbtree.cpp:188` | 1 | `BNodeStore::releaseNode` |
| `tempconflictbtree.cpp:191` | 1 | `needsSplit` |
| `tempconflictbtree.cpp:194`, `204` | 1 each | `BNodeStore::findKey` |
| `tempconflictbtree.cpp:202` | 1 | `cleanExpired` |
| `tempconflictbtree.cpp:203` | 1 | `findSplitCandidate` |
| `tempconflictbtree.cpp:1596` | 1, 3 | `std::iota` on `uint32_t` |
| `tempconflictbtree.cpp:1597`, `1598` | 1 and 3 each | `initializeRootTask`, `prefetchBlock` |
| `tempconflictbtree.cpp:1603` | 1, 3 | `cleanPending` |

The calls in the first part of the table can fail with `std::bad_alloc`
when memory is allocated; no allocation failure was injected. The calls in
the second part are not declared `noexcept`, but their current
implementations contain no operation that throws; no fault injection was
performed.

**Cleanup dispatch (2 items).** Line 1604, items 0 and 2: the closing
brace of `checkReads`, where the IR has, in each instantiation, a `switch`
on the cleanup destination after the destructors of the local vectors,
whose default target is `unreachable`. In each instantiation one item has
every execution and the other none; the item does not correspond to a
source condition.

**Outcomes taken by one instantiation only (24 items).** These outcomes
are taken by `checkReads<16>` and not by `checkReads<32>`, because the
tests that produce them use G = 16 only. llvm-cov counts them as covered.

| Location | gcov items | Outcome taken with G = 16 |
|---|---|---|
| `tempconflictbtree.cpp:986`, `991` | 2 each | A write endpoint (`CheckReadsWithoutReads`) |
| `tempconflictbtree.cpp:995` | 2 | A read of a transaction already too old (`CheckReadsMarksTooOldTransactions`) |
| `tempconflictbtree.cpp:998` | 2 | A read below `reclaimCV` (`CheckReadsMarksTooOldTransactions`) |
| `tempconflictbtree.cpp:1004` | 4; 6 and 7 never executed | A transaction already marked (`CheckReadsSkipsConflictingTransactions`) |
| `tempconflictbtree.cpp:1058` | 2 | A leaf emptied by its compaction |
| `tempconflictbtree.cpp:1104` | 7 | The second line of a key prefetched |
| `tempconflictbtree.cpp:1106` | 2 | The second prefetched line in the same key block (`CheckReadsWithRangeEndInLaterKeyBlock`) |
| `tempconflictbtree.cpp:1118` | 3 | A stored key of length 0 (`CheckReadsWithEmptyKeys`) |
| `tempconflictbtree.cpp:1151` | 3 | A read key of length 0 (`CheckReadsWithEmptyKeys`) |
| `tempconflictbtree.cpp:1156` | 3 | A read key longer than the rest of its line |
| `tempconflictbtree.cpp:1176` | 4; 6 and 7 never executed | An expired or marked separator at a partition step |
| `tempconflictbtree.cpp:1182`, `1317` | 3 each | A marked separator at a partition step |
| `tempconflictbtree.cpp:1259`, `1271` | 3 each | `findPrev` moves to the leaf on the left (`CheckReadsWalksLeftIntoPreviousSubtree`) |
| `tempconflictbtree.cpp:1290` | 2 | A leaf reached with no entries after its compaction (`CheckReadsWithEveryEntryExpired`) |
| `tempconflictbtree.cpp:1491` | 12 | A round runs a task in `FindPrev` (line 1504) |
| `tempconflictbtree.cpp:1594` | 5, 7 | An empty tree, a batch without reads |

**Source outcomes not reachable (19 items).** llvm-cov reports these
outcomes as not covered. They cannot occur while the tree's documented
invariants and the interface preconditions hold.

| Location | gcov items | Outcome | Why it does not occur |
|---|---|---|---|
| `tempconflictbtree.cpp:237` | 0 | `normalizeRoot` loop ends by its condition | Every caller has a root, and the loop sets the root to null only before returning |
| `tempconflictbtree.cpp:590` | 2 | Parent still needs a split after losing an empty child | See [Independent condition checks](#independent-condition-checks) |
| `tempconflictbtree.cpp:601` | 0 | `removeIfEmpty` on an empty internal root (lines 601-604) | Its only caller, `cleanPending`, passes leaves |
| `tempconflictbtree.cpp:1031` | 1, 5 | Read of a too-old transaction during the traversal | Removed by `buildBeginIndexes`; no transaction becomes too old later |
| `tempconflictbtree.cpp:1086` | 1, 2, 5, 6 | Slow prefetch with length 0 or an invalid key | `prefetchStoredKey` returns before for length 0; only live entries and separators are prefetched, separators after being refreshed, and their key blocks have CVs above `reclaimCV`, so they are not reclaimed |
| `tempconflictbtree.cpp:1096` | 0, 2 | Prefetch loop ends by its condition | Line 1104 returns when the second line is done |
| `tempconflictbtree.cpp:1265` | 0, 2 | Expired entry met by `findPrev` | `findPrev` compacts the leaf first, and the cut does not change during the batch |
| `tempconflictbtree.cpp:1301` | 1, 3 | Internal node without children reached (lines 1302-1305) | Only a root leaf may be empty when an operation returns, and `checkReads` removes no child before its traversal ends |
| `tempconflictbtree.cpp:1491` | 0, 8 | `default` in `runRound` (line 1520) | Tasks in the order are never `Empty` or `Done` at the start of a round: `endRound` removes them |
| `tempconflictbtree.cpp:1538` | 1, 5 | Task `Empty` in `endRound` | Set only by line 1302, which is not reachable |

**Source outcomes reachable but not exercised (5 items).** llvm-cov
reports these outcomes as not covered. They follow from inputs that no
test provides.

| Location | gcov items | Outcome | Input that reaches it |
|---|---|---|---|
| `tempconflictbtree.cpp:37` | 2 | The reserve is not reached but the write fits | A pool of fewer than 35 blocks (one 64 KiB page holds 32): each write then reclaims all history it can and is stored |
| `tempconflictbtree.cpp:1215` | 2, 8 | A task resumed in `FindPrev` (lines 1226-1228) | A task waiting in `FindPrev` among those suspended when more than G tasks are active |
| `tempconflictbtree.cpp:1013` | 0, 2 | A read kept and then dropped because its transaction became too old | Two reads of one transaction with different versions, the later one below `reclaimCV` |

On lines 1215 and 1491 the item numbers are those of the `switch`; the
correspondence with the cases comes from the llvm-cov counts, which report
`case Step::FindPrev` of `prefetchAgain` (line 1226) and the `default` of
`runRound` (line 1520) as the only cases never taken, and
`case Step::FindPrev` of `runRound` (line 1504) as taken only with G = 16.

## Limitations

- Paths reachable through the interface are not exercised: a pool smaller
  than the 35-block reserve, and the resumption of a task suspended while
  it searches for a previous live entry. See
  [Uncovered generated branches](#uncovered-generated-branches).
- No allocation-failure injection was performed. The effect of a
  `std::bad_alloc` from memory allocation in `addWrite`, `findKey` or
  `checkReads`, and the state of the tree after it, are not tested.
- `checkReads` is tested with one read version per transaction, as
  `ConflictBatch` builds it; reads of one transaction with different
  versions were not tested.
- `writeBegins`, which `checkReads` fills and does not read, is not
  checked by any test.
- Only the 2048-byte block size was tested; the makefile can build other
  sizes with `BLOCK_SIZE`.
- The prefetch hints are not validated beyond their execution: their
  effect on performance is outside the scope of this component.
- No condition coverage was measured: the toolchain does not provide it.
  The isolation of the conditions rests on the configurations listed
  above and on the llvm-cov counts.
- A build with `NDEBUG` defined removes the invariant checks; such a build
  was not tested.
- Concurrent access, and the rule that no reclamation occurs while key
  bytes are accessed, are not tested.
- The reserve policy after a long key was measured only with the tree
  driven directly (see [Reserve after a long key](#reserve-after-a-long-key)),
  not on an FDB workload.
- Results apply to the source and environment identified above.

## Evidence

- [Test output](evidence/tempconflictbtree-tests.txt): output of
  `make -B test` for the full suite (228 tests: PagePool, KeyStore,
  BNodeStore and TempConflictBTree). It is the same run as
  `pagepool-tests.txt`, `keystore-tests.txt` and `bnodestore-tests.txt`.
- [Coverage summary](evidence/tempconflictbtree-coverage.txt): gcov output
  after running only `./tests.exe /resolver/TempConflictBTree`. The filter
  does not appear in the gcov output; it is recorded in
  [Reproduction](#reproduction). Only the block for
  `tempconflictbtree.cpp` is reported here; the file also lists the
  headers gcov visited.
- Annotated coverage, `evidence/tempconflictbtree.cpp.gcov`: from the same
  gcov run.
- [llvm-cov summary](evidence/tempconflictbtree-llvm-report.txt): llvm-cov
  report after running only
  `build/coverage-llvm/tests.exe /resolver/TempConflictBTree`. The filter
  does not appear in the output.
- llvm-cov annotated coverage, `evidence/tempconflictbtree-llvm-show.txt`:
  `tempconflictbtree.cpp` and `tempconflictbtree.h` with line and branch
  counts, from the same llvm-cov run.

- [Reserve after a long key](evidence/longkey-reclaimcv.txt): output of
  `tests/evidence/longkeyreserve.cpp` at 1, 2, 4 and 8 KiB, with the pools
  of the test and with 64 pages; its header gives the tree and compiler.

The two annotated reports are generated by `tests/coverage.sh` and are not
tracked.
