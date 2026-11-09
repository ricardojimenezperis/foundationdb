# TempConflictBTree validation

## Scope

Validation of the temporal conflict B+ tree: a history of write-conflict
ranges ordered by key, each with the commit version (CV) of the write that
stored it, kept in nodes from `BNodeStore` and keys from `KeyStore`, both
drawing blocks from one `PagePool`. The following are validated:

- Search: descent through internal nodes to the leaf and rank of a key,
  including keys before the first separator, and movement between entries
  across leaves (`findKey`, `nextRank`, `prevRank`).
- Writes (`addWrites`): the accepted write ranges of a batch, applied at
  one CV through two alternating groups of write tasks that start from the
  destination leaves `checkReads` prepared, with the parts of overlapped
  ranges outside each write (left and right exteriors) kept with their
  original CVs, erasure of the ranges it covers across leaves and whole
  subtrees, creation of the root leaf, the split of the destination leaf
  or of its nearest ancestor that needs one before the insertion, so that
  the leaf that receives a write always has two free slots, a new search
  when reclamation or another task changes a task's destination, and the
  room reserved before each write, with the `std::length_error` thrown
  when the pool cannot hold one write.
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
  G = 32 (the explicit instantiation for G = 24 is not run): reads taken from a batch laid out as FoundationDB's
  `ConflictBatch` lays it out, too-old transactions, empty reads,
  transactions already conflicting, reporting transactions with one
  conflict bit per read, partition of the reads among children, tasks
  suspended beyond G and resumed, compaction of each leaf once per batch,
  refresh of expired separators before their keys are used, children
  without live entries marked with `EmptyChildCV` and routed conservatively
  to the left, the search for a previous live entry in the leaves to the
  left, the empty key, and removal after the traversal of the leaves left
  empty.
- Mixed batches, whose transactions read, write or both: the begin
  records that `checkReads` builds for each transaction, the writes of
  transactions whose reads conflict or are too old left out by
  `addWrites`, overlapping write ranges of one transaction merged,
  adjacent and empty ranges, and the reads of later batches against the
  result.
- The single-read check `hasConflict`, a private function that the tests
  use as a reference for `checkReads`.

Outside the scope of this component: `PagePool`, `KeyStore` and
`BNodeStore` themselves (see [PagePoolTests.md](PagePoolTests.md),
[KeyStoreTests.md](KeyStoreTests.md) and
[BNodeStoreTests.md](BNodeStoreTests.md)); the integration of the tree in
the resolver (`ConflictSet.cpp`) and the construction of the batch there;
the benchmark in `bench/`; the write path of a build with
`RESOLVER1_WRITE_LG` set to 0 (`addWritesSequential`, `addWrite`,
`eraseCoveredRanges`), which the tested build compiles but does not call;
performance and the effect of the prefetch hints
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
- G is 16, 24 or 32, the three capacities instantiated in
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

`addWrites` applies the writes of the batch last given to `checkReads`,
whose records and key bytes stay valid for the call. It stores the write
ranges of the transactions that are neither too old nor marked in
`transactionConflictStatus`, merged where they overlap by their sorted
endpoint indices, all with one CV, which must not decrease between calls.
Keys are at most 65,535 bytes long. Before each merged range,
`reserveWrite` reclaims history until the pool has
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
  | `tempconflictbtree.cpp` | `d8d125cf0808140c` |
  | `tempconflictbtree.h` | `836d53adee100a14` |
  | `bnodestore.cpp` | `a945a23136d881e9` |
  | `bnodestore.h` | `c80b90d423d97de6` |
  | `keystore.cpp` | `1e1f08d5dc1e8f49` |
  | `keystore.h` | `7923ad926d27acfc` |
  | `pagepool.cpp` | `66d0fa33d5e1b771` |
  | `pagepool.h` | `86f8dee90b69e43e` |
  | `fdbcompat.h` | `50bf041b46640d1d` |
  | `common.h` | `a815af89662b3a73` |
  | `../ResolverBatchTypes.h` | `44e93cb91d982c40` |
  | `test.h` | `a9a025cd287fd82e` |
  | `testmain.cpp` | `f977afc8cc420126` |
  | `makefile` | `2a8173902bc33339` |
  | `tests/tempconflictbtreetests.cpp` | `3e76fd7f2b6c0a99` |
  | `tests/pagepooltests.cpp` | `059580ab0c6dff71` |
  | `tests/keystoretests.cpp` | `f3c274cf80f60567` |
  | `tests/bnodestoretests.cpp` | `a08ffdb64836e858` |
  | `tests/coverage.sh` | `6efaddf0828ff825` |

- Working tree: the files listed above
- Platform: macOS 27.0.1 (build 26A434), arm64 (Apple M3 Pro)
- Compiler: Apple clang 15.0.0 (clang-1500.3.9.4), target
  `arm64-apple-darwin27.0.0`
- Language standard: C++20
- Build flags: `-I. -std=c++20 -Wall -Wextra -Wpedantic -g`, with the
  default block size of 2048 bytes (`RESOLVER1_BLOCK_SIZE` not defined).
  With this block size `LeafCapacity` is 59 and `InternalCapacity` is 40.
  `RESOLVER1_WRITE_LG` and `RESOLVER1_WRITE_GROUPS` are not defined, so
  `addWrites` uses their defaults, 8 tasks per group and 2 groups.
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
| `/resolver/TempConflictBTree/CleanExpiredRemovesOnlyExpiredEntries` | With `reclaimCV` 10, cleaning a leaf with CVs 5, 15, 3, 20 and 10 keeps exactly the entries with CVs 15 and 20, frees exactly the other slots, raises the leaf's mutation epoch by 3 and reports a change; a second cleaning reports none. | PASS |
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
| `/resolver/TempConflictBTree/SplitRootLeaf` | A full root leaf splits into halves of 29 and 30 entries under a new root with CV 500, linked to each other, each with mutation epoch 1; the new separator is the first begin of the sibling with its CV. For every begin, every key after a begin and a key before the first, the leaf and rank returned by the split are those `findKey` returns on the split tree, and no entry is lost. | PASS |
| `/resolver/TempConflictBTree/SplitLeafUnderParent` | A full leaf under a parent: the sibling is linked between the leaf and its right neighbour and inserted after it in the parent, which stops counting the leaf; positions are kept as above. | PASS |
| `/resolver/TempConflictBTree/SplitLeafUnderParentNeedingSplit` | Splitting a full leaf whose parent needs a split uses the reserved slot: the parent becomes full, counts no child, and is now the node `findKey` returns as needing a split. | PASS |
| `/resolver/TempConflictBTree/SplitRootInternal` | Five layouts of an internal root needing a split (full without counted children; counted children first, last and spread; all counted): the cut is the shortest prefix reaching half the weight, both halves stop needing a split, children keep their order, the sibling's slots are filled in rank order, and leaf and rank are unchanged. | PASS |
| `/resolver/TempConflictBTree/SplitInternalUnderParent` | An internal node under the root: the sibling goes right after it, the root stops counting it, the counted children are shared between the halves, and the sibling's CV reaches the root; every leaf is still reached, and only full leaves are returned as needing a split. | PASS |
| `/resolver/TempConflictBTree/LeafNeedsSplitWithOneFreeSlot` | Leaves with 57, 58 and 59 entries: only the last two need a split, are counted by the parent and are returned by `findKey`. | PASS |
| `/resolver/TempConflictBTree/SplitLeafWithOneFreeSlot` | A leaf with one free slot splits into halves of 29 entries that do not need a split, and every position is kept. | PASS |

Insertion, erasure, detachment and propagation:

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/InsertAtFillsRootLeaf` | An empty root leaf is filled in a scattered order: each insertion raises the leaf's maximum CV and its mutation epoch by 1, the leaf stays the oldest node, and it needs a split from its 58th entry on. | PASS |
| `/resolver/TempConflictBTree/InsertAtFillsLeafUnderParent` | A leaf between two others is filled in a scattered order: its separator follows its first key, the parent's CVs follow the leaf's, and the parent counts the leaf from its 58th entry on. | PASS |
| `/resolver/TempConflictBTree/InsertAtUpdateCombinations` | All eight combinations of the updates an insertion may cause in the parent: the leaf starts needing a split, the separator changes because the entry goes first, the maximum CV rises because the entry is newer. | PASS |
| `/resolver/TempConflictBTree/InsertAtPropagatesSeparator` | An entry inserted first in a leaf changes the separators up to the first ancestor where the subtree is not the first child, for first and non-first leaves of both internal nodes. | PASS |
| `/resolver/TempConflictBTree/InsertAtPropagatesNeedsSplit` | A leaf that starts needing a split under a parent with one free slot makes the parent need one, and the root counts it; a further entry that fills the leaf changes no count. | PASS |
| `/resolver/TempConflictBTree/InsertAtPropagatesMaxCV` | A newer entry raises the maximum CVs up to the root; a second one with the same CV under the same parent stops at the parent. | PASS |
| `/resolver/TempConflictBTree/PropagateUpdatesSeparator` | After removing the first entry of a leaf, propagating the separator copies the new first key and its CV into the parent, and further up while the node is a first child. | PASS |
| `/resolver/TempConflictBTree/PropagateUpdatesSeparatorOfInternalNode` | The same for an internal node's first separator; on the root, and on a root leaf, nothing changes. | PASS |
| `/resolver/TempConflictBTree/EraseAtKeepsLeaf` | Erasing entries that leave their leaf non-empty (middle; first, with the separators following across levels; last with a leaf to the right; last of the last leaf): each erase raises the leaf's mutation epoch by 1, the cursor identifies the next entry or the end position, and the tree matches the model. | PASS |
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
the test helper `addWrite`, which resolves a batch of one write with
`checkReads<16>` and `addWrites`, and checked against a model that replaces what the write covers
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
| `/resolver/TempConflictBTree/AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` | With the production geometry, 2 KiB blocks in a pool of two 64 KiB pages (64 blocks), batches at CV 1 to 37 each write the distinct point keys drawn by a fixed generator at one CV, then read every key at the previous version. Reclamation frees the prepared destination of a write group while another task holds a rank. After every batch the structure is valid and every write of the batch is in the tree; the 4,645 reads give exactly 0 too-old transactions and 4,645 conflicts, with FNV-1a hash `0xe4379ac1a8a77428` of the decisions, at least one admitted read, and a conflict for every admitted read. With other block sizes the body is skipped. | PASS |

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

Mixed batches. Each case builds a history of point writes at CV 10 on
every fourth key, `4 * LeafCapacity` of them, so that the tree has
several leaves at every block size, sets `reclaimCV` to 5, resolves one
batch with `checkReads<G>` and `addWrites` at CV 50, laid out as
`ConflictBatch` lays it out, and runs with G = 16 and G = 32. The oracle
is the behaviour after the batch: the stored entries and the outcomes of
later reads.

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/TempConflictBTree/CheckReadsAndAddWritesResolveAMixedBatch` | Five transactions whose begins interleave in key order: reads only, writes only, a read and a write admitted, a read that conflicts with the history and a read below the cut. The statuses, conflict bits, too-old flags and too-old count are exact; only the writes of the write-only and the admitted transactions are stored, at CV 50, with the history unchanged; later reads at version 45 conflict exactly on those writes and not on the keys around them or on the discarded writes, and reads at version 6 still conflict with the history; each begin record names its own read range or write endpoint and its transaction, and the too-old transaction has none. | PASS |
| `/resolver/TempConflictBTree/AddWritesMergesOverlappingWritesOfAMixedBatch` | An admitted transaction writes two overlapping ranges, a range adjacent to them and an empty range; a read-only transaction; a transaction whose read conflicts writes a range over and beyond them. With equal keys ordered as `ConflictSet.cpp` orders them and with begins first: the entries at CV 50 cover exactly the union, without gaps or overlaps; the history inside it is replaced and the rest, including under the discarded range, is kept; point reads on every key from before the union to past the discarded range conflict only inside it, and reads ending at its begin, starting at its end or over the empty range do not. | PASS |

128 TempConflictBTree tests passed, 0 failed. The full run, including the
15 PagePool tests, the 41 KeyStore tests and the 48 BNodeStore tests,
reports 232 passed and 0 failed.

The tree checks its internal invariants, the results of its block
allocations and the results of the key records a write stores with
`assert`, which terminates the process when it fails and is removed when
`NDEBUG` is defined. The assertion builds do not define `NDEBUG`. Through the
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
| Lines executed | 90.03% of 1906 |
| Branches executed | 91.10% of 1911 |
| Taken at least once | 60.81% of 1911 |

llvm-cov source-based coverage:

| File | Regions | Functions | Lines | Branches |
|---|---|---|---|---|
| `tempconflictbtree.cpp` | 88.98% of 1370 | 93.20% of 103 | 92.07% of 3151 | 85.48% of 992 |
| `tempconflictbtree.h` | 88.89% of 18 | 86.67% of 15 | 55.77% of 52 | 100.00% of 2 |

The classification in this section was made on the tree identified in
[Source and environment](#source-and-environment), from the Apple clang 15
evidence of the same run. Line numbers refer to `tempconflictbtree.cpp`.

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

Two parts of the file are compiled but not run by the tests:

- `addWritesSequential` (lines 538-642), `addWrite` (lines 811-868) and
  `eraseCoveredRanges` (lines 917-932) are the write path of a build with
  `RESOLVER1_WRITE_LG` set to 0. `addWrites` calls `addWritesSequential`
  only in that build (`if constexpr`, line 29). The tested build uses the
  default, 8, with `RESOLVER1_WRITE_GROUPS` 2.
- `checkReads` is explicitly instantiated for G = 16, 24 and 32
  (lines 2529-2554). The tests run G = 16, and G = 32 in
  `CheckReadsAgainstReference` and
  `CheckReadsMatchesHasConflictOnWrittenTree`, on batches of reads, and
  in the two mixed-batch cases; no test runs `checkReads<24>`.

`checkReads` and its lambdas are separate functions in each instantiation.
gcov shows the three instantiations on the same source line, with the
items of `checkReads<16>` first, then `<24>`, then `<32>`: on each such
line the first third of the items belongs to G = 16, the second to G = 24
and the last to G = 32. llvm-cov shows each instantiation separately.
Counting an outcome as covered when any instantiation takes it gives 496
conditions and 992 branches in the file, 144 of them not covered, as in
its report; 129 of the conditions, with 18 uncovered branches, are in
`checkReads`, and 37, with all 74 of their branches uncovered, are in the
three functions of the `RESOLVER1_WRITE_LG` 0 build.

The annotated gcov file marks 502 branch items as never executed and 247
as taken 0 times: 749 items, which is 1911 minus the 1162 taken at least
once. 434 of the never-executed items are in the two parts that are not
run: 136 in the three functions of the other build and 298 in
`checkReads<24>`.

`llvm-cov show` marks with count 0 the lines 30-32, 332-334, 340, 370, 393, 416-417, 542-642 (except blank and brace-only
lines), 655-657, 718-719, 726-734, 739-741, 804-805, 814-868, 920-932,
1426-1430, 1828-1829, 2101-2103, 2161-2162, 2201-2208,
2388-2413 (the closing braces after each `break` in `runRound`, and its
`default`) and 2501-2502; each is accounted for in
[Uncovered generated branches](#uncovered-generated-branches). Lines
30-32 are the branch of `if constexpr (LG == 0)` that the tested build
discards. Lines 340 and 370 are `continue` statements reached only through
outcomes of lines 331, 337 and 366 that do not occur, and line 393 is the
`case Step::Previous` label, which is entered only by `goto`. In
`tempconflictbtree.h`, the two functions not executed are the template
`makePrefetchOrder` (line 55), which is evaluated at compile time, and
`RangeEntry::setWrite` (line 137), which no code calls.

### Independent condition checks

`tempconflictbtree.cpp` has 65 compound decisions outside the `assert`
checks and outside the three functions of the `RESOLVER1_WRITE_LG` 0
build, with 145 conditions. The table lists each condition with its
llvm-cov counts, summed over the instantiations of `checkReads`, and, for
each outcome, the first test in registration order whose run alone
produces it. The tests were run one at a time, each with its own
profile, on the same build as the evidence, 128 runs; the three test names that
are a prefix of another name also ran that test. A named test shows that
the outcome occurs; it does not show that the condition alone decided the
outcome, which would need condition coverage that the toolchain does not
provide.

The counts assume the values of the tested platform and build:
`BlockSize` 2048, `sizeof(KeyBlockHeader)` 24, `LeafCapacity` 59,
`InternalCapacity` 40, 32 blocks per 64 KiB page, `RESOLVER1_WRITE_LG` 8,
`RESOLVER1_WRITE_GROUPS` 2, and `EmptyChildCV` equal to -1, below every
`reclaimCV_`, which starts at 0 and never decreases. The prefetch code
uses a fixed line size of 64 bytes, which is not read from the platform.

An outcome with no test is not covered; each is classified in
[Uncovered generated branches](#uncovered-generated-branches) at the line
of its gcov items.

| Location | Condition | True | False | A test with true | A test with false |
|---|---|---|---|---|---|
| `tempconflictbtree.cpp:59` | `!info.tooOld` | 267.1k | 0 | `AddWriteKinds` | - |
| `tempconflictbtree.cpp:59` | `!transactionConflictStatus[entry.transaction]` | 267.1k | 6 | `AddWriteKinds` | `CheckReadsAndAddWritesResolveAMixedBatch` |
| `tempconflictbtree.cpp:66` | `pending != endWrites` | 5.28k | 261.8k | `AddWritesAgainstReference` | `AddWriteKinds` |
| `tempconflictbtree.cpp:66` | `entry.index() <= readWriteBegins[pending].transaction` | 6 | 5.28k | `AddWritesMergesOverlappingWritesOfAMixedBatch` | `AddWritesAgainstReference` |
| `tempconflictbtree.cpp:134` | `t.write.first.size() != 0` | 267.1k | 0 | `AddWriteKinds` | - |
| `tempconflictbtree.cpp:134` | `t.write.second.size() != 0` | 0 | 0 | - | - |
| `tempconflictbtree.cpp:143` | `!t.hasSavedLeaf` | 0 | 1.33M | - | `AddWriteKinds` |
| `tempconflictbtree.cpp:143` | `t.savedLeaf == nullptr` | 955 | 1.33M | `AddWriteAgainstModel` | `AddWriteKinds` |
| `tempconflictbtree.cpp:145` | `(generation & 1) != 0` | 1.33M | 0 | `AddWriteKinds` | - |
| `tempconflictbtree.cpp:145` | `generation == t.savedLeafGeneration` | 1.33M | 0 | `AddWriteKinds` | - |
| `tempconflictbtree.cpp:146` | `t.savedLeaf->mutationEpoch == t.savedLeafEpoch` | 1.33M | 20 | `AddWriteKinds` | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` |
| `tempconflictbtree.cpp:153` | `destination.leaf != nullptr` | 262.5k | 249 | `AddWriteKinds` | `AddWriteAgainstModel` |
| `tempconflictbtree.cpp:153` | `destination.needsRightCheck()` | 0 | 262.5k | - | `AddWriteKinds` |
| `tempconflictbtree.cpp:165` | `g.count < TaskCapacity` | 1.05M | 79 | `AddWriteKinds` | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` |
| `tempconflictbtree.cpp:165` | `nextGroup < writeLeaves.size()` | 262.6k | 785.5k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:231` | `t.scan->header.count == 1` | 2 | 670 | `AddWriteKinds` | `AddWriteAgainstModel` |
| `tempconflictbtree.cpp:231` | `hintLeaf(t.scan->left)` | 2 | 0 | `AddWriteKinds` | - |
| `tempconflictbtree.cpp:245` | `representative != t.representative` | 28 | 266.3k | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` | `AddWriteKinds` |
| `tempconflictbtree.cpp:245` | `anchor != t.anchor` | 18 | 266.3k | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` | `AddWriteKinds` |
| `tempconflictbtree.cpp:246` | `writePreparation_[representative].mutation != t.mutation` | 37 | 266.2k | `AddWritesAgainstReference` | `AddWriteKinds` |
| `tempconflictbtree.cpp:246` | `reclaimCV_ != t.cutoff` | 119 | 266.1k | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` | `AddWriteKinds` |
| `tempconflictbtree.cpp:247` | `!savedLeafIsCurrent(t)` | 20 | 266.1k | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` | `AddWriteKinds` |
| `tempconflictbtree.cpp:286` | `before != t.cursor.leaf` | 2 | 267.1k | `AddWritesThrowsWhenThePoolCannotHoldAWrite` | `AddWriteKinds` |
| `tempconflictbtree.cpp:286` | `cutoff != reclaimCV_` | 861 | 266.3k | `AddWritesReclaimsRoomForLongKeys` | `AddWriteKinds` |
| `tempconflictbtree.cpp:287` | `!savedLeafIsCurrent(t)` | 0 | 266.3k | - | `AddWriteKinds` |
| `tempconflictbtree.cpp:304` | `!head` | 643 | 266.4k | `AddWritesAgainstReference` | `AddWriteKinds` |
| `tempconflictbtree.cpp:304` | `cleanLeafCV_ != reclaimCV_` | 0 | 643 | - | `AddWritesAgainstReference` |
| `tempconflictbtree.cpp:305` | `leaf->lastCleanReadBatch != readBatch_` | 38 | 605 | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` | `AddWritesAgainstReference` |
| `tempconflictbtree.cpp:305` | `t.cursor.rightCheckRequired` | 0 | 605 | - | `AddWritesAgainstReference` |
| `tempconflictbtree.cpp:308` | `head` | 266.4k | 605 | `AddWriteKinds` | `AddWritesAgainstReference` |
| `tempconflictbtree.cpp:308` | `cleanExpired(leaf)` | 136 | 266.3k | `AddWritesKeepsTheReserveOfALongKeyAfterReclaimingIt` | `AddWriteKinds` |
| `tempconflictbtree.cpp:329` | `t.scanRank < t.scan->header.count` | 298 | 0 | `AddWritesAgainstReference` | - |
| `tempconflictbtree.cpp:330` | `t.scan->cv[t.scan->order[t.scanRank]] <= reclaimCV_` | 17 | 281 | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` | `AddWritesAgainstReference` |
| `tempconflictbtree.cpp:397` | `t.cursor.leaf == nullptr` | 191 | 266.9k | `AddWriteAgainstModel` | `AddWriteKinds` |
| `tempconflictbtree.cpp:397` | `t.found` | 176.4k | 90.5k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:404` | `t.scan->cv[slot] > reclaimCV_` | 90.2k | 0 | `AddWriteKinds` | - |
| `tempconflictbtree.cpp:404` | `hintKey(t, t.scan->ranges[slot], true, Step::Insert)` | 90.2k | 0 | `AddWriteKinds` | - |
| `tempconflictbtree.cpp:422` | `t.scan->cv[slot] > reclaimCV_` | 20 | 1 | `AddWriteInsideRangeOfPreviousLeaf` | `AddWriteAfterReclaimedPreviousKey` |
| `tempconflictbtree.cpp:422` | `hintKey(t, t.scan->ranges[slot], true, Step::Insert)` | 20 | 0 | `AddWriteInsideRangeOfPreviousLeaf` | - |
| `tempconflictbtree.cpp:500` | `which != 0` | 2.28M | 2.28M | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:500` | `headGroup.first < (headGroup.count + 1) / 2` | 2.01M | 277.9k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:515` | `which == 0` | 2.28M | 2.04k | `AddWriteKinds` | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` |
| `tempconflictbtree.cpp:515` | `i == group.first` | 2.28M | 1.55k | `AddWriteKinds` | `AddWritesAgainstReference` |
| `tempconflictbtree.cpp:533` | `!writePreparation_.empty()` | 457.2k | 2.11k | `AddWriteKinds` | `CleanExpiredRemovesOnlyExpiredEntries` |
| `tempconflictbtree.cpp:533` | `leaf->firstWrite != UINT32_MAX` | 453.4k | 3.75k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:678` | `nextOther[group.firstWrite] == g` | 262.5k | 38 | `AddWriteKinds` | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` |
| `tempconflictbtree.cpp:678` | `group.leaf != nullptr` | 262.3k | 194 | `AddWriteKinds` | `AddWriteAgainstModel` |
| `tempconflictbtree.cpp:687` | `activeWriteCursor_ != nullptr` | 862 | 105 | `AddWriteKinds` | `RefreshSeparatorKeepsFreshSeparator` |
| `tempconflictbtree.cpp:687` | `activeWriteCursor_->leaf == leaf` | 25 | 837 | `AddWritesThrowsWhenThePoolCannotHoldAWrite` | `AddWriteKinds` |
| `tempconflictbtree.cpp:693` | `!writeDestinationsActive_` | 105 | 862 | `RefreshSeparatorKeepsFreshSeparator` | `AddWriteKinds` |
| `tempconflictbtree.cpp:693` | `leaf->firstWrite == UINT32_MAX` | 819 | 43 | `AddWriteKinds` | `AddWritesThrowsWhenThePoolCannotHoldAWrite` |
| `tempconflictbtree.cpp:701` | `replacement != nullptr` | 40 | 3 | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` | `AddWritesThrowsWhenThePoolCannotHoldAWrite` |
| `tempconflictbtree.cpp:701` | `replacement->firstWrite != UINT32_MAX` | 38 | 2 | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` |
| `tempconflictbtree.cpp:712` | `!writeDestinationsActive_` | 209 | 846 | `DetachLeaf` | `AddWritesThrowsWhenThePoolCannotHoldAWrite` |
| `tempconflictbtree.cpp:712` | `activeWriteCursor_ == nullptr` | 209 | 0 | `DetachLeaf` | - |
| `tempconflictbtree.cpp:759` | `writeLeafBufferFirst_ < writeLeafBufferNext_` | 263.1k | 261.7k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:760` | `writeLeafBuffer_[writeLeafBufferHead_].released` | 262.4k | 721 | `AddWriteKinds` | `AddWritesAgainstReference` |
| `tempconflictbtree.cpp:775` | `!reclaimUntilAvail(std::max(MinWriteBlocks, writeBlocks))` | 1 | 267.1k | `AddWritesThrowsWhenThePoolCannotHoldAWrite` | `AddWriteKinds` |
| `tempconflictbtree.cpp:776` | `pool_.availableBlocks() < writeBlocks` | 1 | 0 | `AddWritesThrowsWhenThePoolCannotHoldAWrite` | - |
| `tempconflictbtree.cpp:803` | `leaf == nullptr` | 191 | 266.9k | `AddWriteAgainstModel` | `AddWriteKinds` |
| `tempconflictbtree.cpp:803` | `root_ != nullptr` | 0 | 191 | - | `AddWriteAgainstModel` |
| `tempconflictbtree.cpp:883` | `!found` | 90.5k | 176.4k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:883` | `prevRank(previousLeaf, previous, slot)` | 90.2k | 307 | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:885` | `hasPrevious` | 90.2k | 176.7k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:885` | `previousLeaf->cv[slot] <= reclaimCV_` | 1 | 90.2k | `AddWriteAfterReclaimedPreviousKey` | `AddWriteKinds` |
| `tempconflictbtree.cpp:886` | `hasPrevious` | 90.2k | 176.7k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:886` | `!expired` | 90.2k | 1 | `AddWriteKinds` | `AddWriteAfterReclaimedPreviousKey` |
| `tempconflictbtree.cpp:887` | `keyStore_.compareEnd(write.first, previousLeaf->ranges[slot]) < 0` | 2.63k | 87.6k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:938` | `leaf == nullptr` | 1 | 229.2k | `PreserveRightExteriorUpdatesSeparator` | `AddWriteKinds` |
| `tempconflictbtree.cpp:938` | `rank >= leaf->header.count` | 1 | 229.2k | `PreserveRightExteriorUpdatesSeparator` | `AddWriteKinds` |
| `tempconflictbtree.cpp:943` | `keyStore_.compareBegin(end, oldRef) <= 0` | 228.0k | 1.17k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:944` | `keyStore_.compareEnd(end, oldRef) >= 0` | 2 | 1.17k | `PreserveRightExteriorUpdatesSeparator` | `AddWriteKinds` |
| `tempconflictbtree.cpp:995` | `!found` | 1.77M | 5.32k | `FindKeyThroughOneInternalLevel` | `FindKeyThroughOneInternalLevel` |
| `tempconflictbtree.cpp:995` | `rank > 0` | 1.76M | 10.4k | `FindKeyThroughOneInternalLevel` | `FindKeyBeforeFirstSeparator` |
| `tempconflictbtree.cpp:1035` | `!splitBefore` | 268.8k | 20 | `InsertAtFillsRootLeaf` | `InsertAtFillsRootLeaf` |
| `tempconflictbtree.cpp:1035` | `splitAfter` | 2.83k | 266.0k | `InsertAtFillsRootLeaf` | `InsertAtFillsRootLeaf` |
| `tempconflictbtree.cpp:1050` | `root_->kind != BNodeKind::Internal` | 10 | 934 | `AddWriteKinds` | `DeferredRefreshMarksEmptyLeaf` |
| `tempconflictbtree.cpp:1050` | `root_->count != 1` | 918 | 16 | `DeferredRefreshMarksEmptyLeaf` | `DeferredRefreshMarksEmptyInternalChild` |
| `tempconflictbtree.cpp:1066` | `splitBefore` | 300 | 179.8k | `AddWriteAgainstModel` | `AddWriteKinds` |
| `tempconflictbtree.cpp:1066` | `!needsSplit(leaf)` | 299 | 1 | `AddWriteAgainstModel` | `EraseAtKeepsSplitOfFullLeaf` |
| `tempconflictbtree.cpp:1067` | `rank == 0` | 861 | 179.2k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:1067` | `leaf->header.count > 0` | 836 | 25 | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:1083` | `oldParent->header.count == 0` | 6 | 23 | `AddWriteErasesWholeTree` | `AddWriteKinds` |
| `tempconflictbtree.cpp:1083` | `oldParent->parent() != nullptr` | 5 | 1 | `AddWriteErasesWholeTree` | `EraseAtRemovesChainToRoot` |
| `tempconflictbtree.cpp:1133` | `node->header.count == 0` | 43 | 874 | `DetachRemovesEmptyParents` | `DetachLeaf` |
| `tempconflictbtree.cpp:1133` | `node->parent() != nullptr` | 42 | 1 | `DetachRemovesEmptyParents` | `DetachEmptiesTree` |
| `tempconflictbtree.cpp:1199` | `!keyCV` | 7 | 1.19k | `ReclaimOldestWithNothingToReclaim` | `HasConflictSkipsReclaimedKeysToTheRight` |
| `tempconflictbtree.cpp:1199` | `!nodeCV` | 6 | 1 | `ReclaimOldestWithNothingToReclaim` | `ReclaimOldestOnlyNodes` |
| `tempconflictbtree.cpp:1236` | `child->header.count > 0` | 49 | 5 | `RemoveEmptyChildPropagatesSplitCleared` | `RemoveEmptyChildPropagatesSplitCleared` |
| `tempconflictbtree.cpp:1237` | `child->separatorsCV[child->order[0]] <= reclaimCV_` | 48 | 1 | `RemoveEmptyChildPropagatesSplitCleared` | `RefreshSeparatorSkipsExpiredGrandchildren` |
| `tempconflictbtree.cpp:1387` | `splitBefore` | 3.62k | 330.6k | `RefreshSeparatorRemovesEmptyLeaf` | `CleanExpiredRemovesOnlyExpiredEntries` |
| `tempconflictbtree.cpp:1387` | `!needsSplit(leaf)` | 8 | 3.61k | `RefreshSeparatorRemovesEmptyLeaf` | `CleanExpiredReportsLeafTransitionOnce` |
| `tempconflictbtree.cpp:1414` | `splitBefore` | 2 | 172 | `RemoveEmptyChildPropagatesSplitCleared` | `RefreshSeparatorKeepsFreshSeparator` |
| `tempconflictbtree.cpp:1414` | `!needsSplit(parent)` | 2 | 0 | `RemoveEmptyChildPropagatesSplitCleared` | - |
| `tempconflictbtree.cpp:1434` | `parent->header.count == 0` | 4 | 32 | `DeferredRefreshMarksEmptyInternalChild` | `DeferredRefreshMarksEmptyLeaf` |
| `tempconflictbtree.cpp:1434` | `parent->parent() != nullptr` | 3 | 1 | `DeferredRefreshMarksEmptyInternalChild` | `FirstLiveKey` |
| `tempconflictbtree.cpp:1519` | `activeWriteCursor_ != nullptr` | 2.52k | 357 | `AddWriteAgainstModel` | `SplitRootLeaf` |
| `tempconflictbtree.cpp:1519` | `activeWriteCursor_->leaf == oldNode` | 2.52k | 0 | `AddWriteAgainstModel` | - |
| `tempconflictbtree.cpp:1522` | `writeDestinationsActive_` | 2.52k | 357 | `AddWriteAgainstModel` | `SplitRootLeaf` |
| `tempconflictbtree.cpp:1522` | `oldNode->firstWrite != UINT32_MAX` | 2.52k | 5 | `AddWriteAgainstModel` | `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` |
| `tempconflictbtree.cpp:1554` | `rank > cutRank` | 2.16k | 714 | `SplitRootLeaf` | `SplitRootLeaf` |
| `tempconflictbtree.cpp:1554` | `found` | 193 | 521 | `SplitRootLeaf` | `SplitRootLeaf` |
| `tempconflictbtree.cpp:1554` | `rank == cutRank` | 6 | 187 | `SplitRootLeaf` | `SplitRootLeaf` |
| `tempconflictbtree.cpp:1623` | `parent != nullptr` | 540.5k | 251.1k | `RefreshSeparatorRemovesEmptyLeaf` | `RefreshSeparatorRemovesEmptyLeaf` |
| `tempconflictbtree.cpp:1623` | `splitDelta != 0` | 3.25k | 537.2k | `RefreshSeparatorRemovesEmptyLeaf` | `RemoveEmptyChildPropagatesSplitCleared` |
| `tempconflictbtree.cpp:1623` | `updateSeparator` | 7.44k | 529.8k | `DeferredRefreshThroughInternalChild` | `RemoveEmptyChildPropagatesSplitCleared` |
| `tempconflictbtree.cpp:1623` | `updateCV` | 327.1k | 202.7k | `SplitRootLeaf` | `RemoveEmptyChildPropagatesSplitCleared` |
| `tempconflictbtree.cpp:1671` | `slotPresent` | 1.18M | 55.2k | `HasConflictAroundOneRange` | `HasConflictAroundOneRange` |
| `tempconflictbtree.cpp:1671` | `leaf->cv[slot] > read.version` | 716.0k | 464.8k | `HasConflictAroundOneRange` | `HasConflictAroundOneRange` |
| `tempconflictbtree.cpp:1672` | `keyStore_.compareEnd(read.begin, leaf->ranges[slot]) < 0` | 29.9k | 686.1k | `HasConflictAroundOneRange` | `HasConflictAroundOneRange` |
| `tempconflictbtree.cpp:1839` | `!info.readRanges.empty()` | 143.2k | 267.1k | `CheckReadsInEmptyTree` | `AddWriteKinds` |
| `tempconflictbtree.cpp:1840` | `reads[offsets[t]].version < reclaimCV_` | 4 | 143.2k | `CheckReadsMarksTooOldTransactions` | `CheckReadsInEmptyTree` |
| `tempconflictbtree.cpp:1856` | `transactionConflictStatus[t]` | 6 | 143.2k | `CheckReadsSkipsConflictingTransactions` | `CheckReadsInEmptyTree` |
| `tempconflictbtree.cpp:1857` | `read.conflictingKeyRange == nullptr` | 3 | 3 | `CheckReadsSkipsConflictingTransactions` | `CheckReadsSkipsConflictingTransactions` |
| `tempconflictbtree.cpp:1878` | `lastRead == pointsSize` | 261.8k | 281 | `AddWriteKinds` | `CheckReadsInEmptyTree` |
| `tempconflictbtree.cpp:1878` | `lastWrite == pointsSize` | 275 | 6 | `CheckReadsInEmptyTree` | `CheckReadsAndAddWritesResolveAMixedBatch` |
| `tempconflictbtree.cpp:1904` | `transactionInfo[entry.transaction]->tooOld` | 0 | 677.2k | - | `CheckReadsAroundOneRange` |
| `tempconflictbtree.cpp:1905` | `conflicts[entry.rangeIndex]` | 376 | 676.8k | `CheckReadsSkipsEmptyReads` | `CheckReadsAroundOneRange` |
| `tempconflictbtree.cpp:1906` | `!transactionConflictStatus[entry.transaction]` | 625.4k | 51.4k | `CheckReadsAroundOneRange` | `CheckReadsSkipsConflictingTransactions` |
| `tempconflictbtree.cpp:1907` | `reads[entry.rangeIndex].conflictingKeyRange != nullptr` | 30.1k | 21.3k | `CheckReadsSkipsConflictingTransactions` | `CheckReadsReportsEveryConflictingRead` |
| `tempconflictbtree.cpp:1961` | `length == 0` | 0 | 1.78k | - | `ScatteredPointsRegression` |
| `tempconflictbtree.cpp:1961` | `!keyStore_.isValid(range)` | 0 | 1.78k | - | `ScatteredPointsRegression` |
| `tempconflictbtree.cpp:1979` | `length == 0` | 1.77k | 1.77k | `ScatteredPointsRegression` | `ScatteredPointsRegression` |
| `tempconflictbtree.cpp:1979` | `line == 1` | 5 | 1.77k | `CheckReadsWithLongKeys` | `ScatteredPointsRegression` |
| `tempconflictbtree.cpp:2051` | `node->separatorsCV[slot] <= reclaimCV_` | 121 | 1.29M | `CheckReadsAgainstReference` | `AddWriteKinds` |
| `tempconflictbtree.cpp:2052` | `node->separatorsCV[slot] != EmptyChildCV` | 115 | 6 | `CheckReadsAgainstReference` | `CheckReadsRoutesPastEmptyChildMarks` |
| `tempconflictbtree.cpp:2160` | `task.rightCheckRequired` | 0 | 262.4k | - | `AddWriteKinds` |
| `tempconflictbtree.cpp:2160` | `leaf == nullptr` | 0 | 262.4k | - | `AddWriteKinds` |
| `tempconflictbtree.cpp:2160` | `leaf->header.count == 0` | 0 | 262.4k | - | `AddWriteKinds` |
| `tempconflictbtree.cpp:2295` | `task.readIndex < task.opEnd` | 3.04k | 140 | `CheckReadsSkipsConflictingTransactions` | `CheckReadsSkipsEmptyReads` |
| `tempconflictbtree.cpp:2296` | `!readStillNeeded(task.readIndex)` | 426 | 2.62k | `CheckReadsAgainstReference` | `CheckReadsSkipsConflictingTransactions` |
| `tempconflictbtree.cpp:2365` | `task.readIndex < task.opEnd` | 12.0k | 27 | `CheckReadsAroundOneRange` | `CheckReadsSkipsEmptyReads` |
| `tempconflictbtree.cpp:2366` | `!readStillNeeded(task.readIndex)` | 1.96k | 9.99k | `CheckReadsSkipsEmptyReads` | `CheckReadsAroundOneRange` |
| `tempconflictbtree.cpp:2426` | `next == operationCount` | 261.7k | 722 | `AddWriteKinds` | `AddWritesAgainstReference` |
| `tempconflictbtree.cpp:2426` | `readWriteBegins[next].isWrite()` | 714 | 8 | `AddWritesAgainstReference` | `CheckReadsAndAddWritesResolveAMixedBatch` |
| `tempconflictbtree.cpp:2434` | `!completedTasks` | 1.59M | 267.4k | `AddWriteKinds` | `AddWriteKinds` |
| `tempconflictbtree.cpp:2434` | `newTasks.empty()` | 1.59M | 3.23k | `AddWriteKinds` | `CheckReadsReportsEveryConflictingRead` |
| `tempconflictbtree.cpp:2444` | `tasks[slot].step == Step::Empty` | 0 | 397.3k | - | `AddWriteKinds` |
| `tempconflictbtree.cpp:2444` | `tasks[slot].step == Step::Done` | 272.4k | 124.9k | `AddWriteKinds` | `CheckReadsReportsEveryConflictingRead` |
| `tempconflictbtree.cpp:2450` | `created < newTasks.size()` | 69.7k | 327.7k | `CheckReadsReportsEveryConflictingRead` | `AddWriteKinds` |
| `tempconflictbtree.cpp:2450` | `newTasks[created].parent == t` | 10.5k | 59.2k | `CheckReadsReportsEveryConflictingRead` | `CheckReadsAgainstReference` |
| `tempconflictbtree.cpp:2486` | `active < G` | 269.9k | 5.06k | `AddWriteKinds` | `CheckReadsAgainstReference` |
| `tempconflictbtree.cpp:2486` | `!pending.empty()` | 4.31k | 265.6k | `CheckReadsAgainstReference` | `AddWriteKinds` |
| `tempconflictbtree.cpp:2515` | `root_ == nullptr` | 193 | 261.9k | `AddWriteAgainstModel` | `AddWriteKinds` |
| `tempconflictbtree.cpp:2515` | `readWriteBegins.empty()` | 0 | 261.9k | - | `AddWriteKinds` |

llvm-cov reports both outcomes covered for every other condition of the
file outside the three functions of the `RESOLVER1_WRITE_LG` 0 build.

### Uncovered generated branches

gcov reports 749 untaken branch items: 502 never executed and 247 taken 0
times. Each item was matched to its edge in the LLVM IR that Apple clang
15 generates for `tempconflictbtree.cpp` at `-O0` with `-g`: llvm-cov
gcov lists the out-edges of a block at the last source line of the block,
and on 510 of the 514 lines with branch items the number of items equals
the number of edges of the conditional branches, `invoke` instructions and
`switch` instructions whose block ends on that line. On the other four
(329, 1743, 2090 and 2501), and on two lines with such edges but no items
(1797 and 2526), the attribution of blocks to lines differs; of them, only
lines 2090 and 2501 have untaken items, and they are classified from the
llvm-cov counts. The outcome of a source
condition was taken from the llvm-cov counts of the same line and
instantiation, not from the order of the items: gcov orders the out-edges
of a block by their destination. Item numbers are those of the annotated
gcov file.

| Class | Items |
|---|---|
| Code of the `RESOLVER1_WRITE_LG` 0 build | 136 |
| Instantiation `checkReads<24>`, not run | 298 |
| Precondition and invariant checks | 110 |
| Exception paths of calls that can throw | 28 |
| Exception paths of calls with no throwing operation | 54 |
| Outcomes taken by `checkReads<16>` only | 22 |
| Source outcomes not reachable | 47 |
| Source outcomes reachable but not exercised | 44 |
| Source outcomes whose reachability was not determined | 10 |
| Total | 749 |

**Code of the `RESOLVER1_WRITE_LG` 0 build (136 items).** Every item on
the 55 lines with branches of `addWritesSequential` (lines 538-642),
`addWrite` (lines 811-868) and `eraseCoveredRanges` (lines 917-932). All
are never executed: the tested build does not call these functions (see
[Coverage](#coverage)).

**Instantiation `checkReads<24>` (298 items).** The middle third of the
items on each line of `checkReads` and its lambdas (lines 1760-2526). All
are never executed: no test calls `checkReads<24>`. It differs from the
other two instantiations only in the value of `G`.

**Precondition and invariant checks (110 items).** The paths to
`__assert_rtn` taken when an `assert` fails. The IR contains a call to
`__assert_rtn` on each of these lines. Where the call is an `invoke`, in
a function with objects to destroy, both of its edges are listed and are
never executed.

| Location | gcov items |
|---|---|
| `advance` (lines 235-518) | 235: 1; 236: 3; 260: 0, 2, 3; 275: 0, 2, 4, 5; 363: 0, 2, 3; 387: 0, 2, 3; 388: 2, 4, 5; 395: 0, 2, 3; 396: 2, 4, 5; 428: 0, 2, 3; 429: 2, 4, 5; 431: 0, 2, 3; 479: 0, 2, 3; 518: 0, 2, 4, 5 |
| Write destinations and the write-leaf buffer (lines 644-753) | 644: 0; 661: 0, 2; 686: 1; 696: 0; 724: 1; 745: 0, 3; 750: 1; 753: 2 |
| Exteriors and the root leaf (lines 879-960) | 879: 1; 892: 1; 899: 0; 948: 0; 958: 1; 960: 0 |
| `findKey`, `eraseAt`, `removeEmptyChild` (lines 984-1398) | 984: 0, 2, 3; 1099: 0; 1130: 0; 1398: 0 |
| Splits and `insertSibling` (lines 1451-1602) | 1451: 1; 1452: 1; 1454: 1; 1460: 0; 1471: 1; 1483: 1; 1484: 0; 1526: 1; 1530: 1; 1531: 0; 1563: 0; 1579: 1; 1580: 1; 1585: 1; 1597: 1; 1598: 1; 1602: 1 |
| `propagateUpdates` (lines 1611, 1629) | 1611: 0, 2; 1629: 3 |
| `checkReads`, G = 16 and G = 32 (lines 1913-2523) | 1913: 1, 5; 1916: 0, 4; 2164: 0, 4; 2425: 0, 2, 8, 10; 2455: 0, 4; 2476: 1, 5; 2487: 1, 5; 2522: 0, 2, 3, 8, 10, 11; 2523: 0, 2, 3, 8, 10, 11 |

On lines 388, 396 and 429 the condition calls `savedLeafIsCurrent`
through an `invoke`; its exception edge, item 1, is listed with the
exception paths below. The checks of the records a write stores (lines
431, 892, 899 and 948) and of node allocations (lines 960, 1471 and 1526)
fail only when the pool runs out of blocks during a write, which the room
`reserveWrite` reserves before the write excludes.

**Exception paths of calls that can throw (28 items).** Exception edges
of `invoke` instructions whose callee can throw in the current code: it
allocates memory (`std::bad_alloc`), or it reaches a `throw`, directly or
through the functions it calls in `tempconflictbtree.cpp`, `keystore.cpp`,
`bnodestore.cpp` and `pagepool.cpp`. The callees that throw in the code
are `appendRange`, through the `std::vector` of `appendRecord` and the
`std::overflow_error` of an exhausted generation, `createRootLeaf` and the
splits, through the `std::overflow_error` of `BNodeStore`,
`prepareWriteSplit`, through the `std::logic_error` of line 804, and
`buildBeginIndexes`, through the `std::length_error` of line 1828. No
allocation failure or exception was injected.

| Location | gcov items | Call |
|---|---|---|
| `tempconflictbtree.cpp:49` | 1 | `std::vector::assign` |
| `tempconflictbtree.cpp:389` | 1 | `prepareWriteSplit` |
| `tempconflictbtree.cpp:430` | 1 | `KeyStore::appendRange` |
| `tempconflictbtree.cpp:433` | 1 | `createRootLeaf` |
| `tempconflictbtree.cpp:437` | 1 | `insertWithLeftExterior` |
| `tempconflictbtree.cpp:467` | 1 | `preserveRightExterior` |
| `tempconflictbtree.cpp:777` | 1 | `std::length_error` constructor, which copies its message |
| `tempconflictbtree.cpp:992` | 1 | `std::vector::push_back` |
| `tempconflictbtree.cpp:1810`, `1821` to `1824` | 1 and 5 each | `std::vector::reserve` |
| `tempconflictbtree.cpp:1813` | 1, 5 | `std::vector::assign` |
| `tempconflictbtree.cpp:2499` | 1, 5 | `buildBeginIndexes` |
| `tempconflictbtree.cpp:2512` | 1, 5 | `std::vector::reserve` |
| `tempconflictbtree.cpp:2519` | 1, 5 | `runRound`, which buffers write leaves and queues leaves for cleanup |
| `tempconflictbtree.cpp:2520` | 1, 5 | `endRound`, which suspends tasks and emits write leaves |

**Exception paths of calls with no throwing operation (54 items).**
Exception edges of `invoke` instructions whose callee is not declared
`noexcept` but, in the current code, reaches no `throw` and no
allocation. No fault injection was performed.

| Location | gcov items | Call |
|---|---|---|
| `tempconflictbtree.cpp:15`, `16` | 1, 0 | `KeyStore` and `BNodeStore` constructors |
| `tempconflictbtree.cpp:55`, `60`, `62`, `64`, `65`, `66` | 1, 1, 1, 1, 1, 3 | `WriteLeaf::end`, `RangeEntry::index`, `VectorRef::begin`, `VectorRef::operator[]` |
| `tempconflictbtree.cpp:68` | 1 | `std::max` on `uint32_t` values |
| `tempconflictbtree.cpp:109` | 1 | Construction of the task groups |
| `tempconflictbtree.cpp:276` | 1 | `StringRef::compare` |
| `tempconflictbtree.cpp:287`, `388`, `396`, `429` | 1 each | `savedLeafIsCurrent` |
| `tempconflictbtree.cpp:308`, `348`, `1002` | 3, 1, 1 | `cleanExpired` |
| `tempconflictbtree.cpp:337`, `366`, `404`, `422`, `461` | 1, 1, 3, 3, 1 | `hintKey` |
| `tempconflictbtree.cpp:342`, `372`, `466` | 1 each | `KeyStore::compareBegin`, `KeyStore::compareEnd` |
| `tempconflictbtree.cpp:434` | 1 | `insertAt` |
| `tempconflictbtree.cpp:278`, `436`, `439`, `451`, `468` | 1 each | `finishInterval` |
| `tempconflictbtree.cpp:462`, `469` | 1 each | `prepareErase` |
| `tempconflictbtree.cpp:472` | 1 | `eraseAt` |
| `tempconflictbtree.cpp:481` | 1 | `hintKeyChunk` |
| `tempconflictbtree.cpp:488`, `525` | 3, 1 | `fill` |
| `tempconflictbtree.cpp:982` | 1 | `refreshSeparators` |
| `tempconflictbtree.cpp:988` | 1 | `BNodeStore::releaseNode` |
| `tempconflictbtree.cpp:991` | 1 | `needsSplit` |
| `tempconflictbtree.cpp:994`, `1004` | 1 each | `BNodeStore::findKey` |
| `tempconflictbtree.cpp:1003` | 1 | `findSplitCandidate` |
| `tempconflictbtree.cpp:2506` | 1, 9 | `RangeEntry::isWrite` |
| `tempconflictbtree.cpp:2516`, `2517` | 1 and 5 each | `initializeRootTask`, `prefetchBlock` |
| `tempconflictbtree.cpp:2524`, `2525` | 1 and 5 each | `prepareWriteDestinations`, `cleanPending` |

**Outcomes taken by `checkReads<16>` only (22 items).** These outcomes
are taken by `checkReads<16>` and not by `checkReads<32>`. The tests that
run G = 32 check random batches of reads on non-empty trees, and the two
mixed batches, which have no transaction already conflicting before the
call, no empty key and no expired entry. llvm-cov counts them as covered.

| Location | gcov items | Outcome taken with G = 16 |
|---|---|---|
| `tempconflictbtree.cpp:1856` | 8; 10 and 11 never executed | A read of a transaction already conflicting |
| `tempconflictbtree.cpp:1875`, `1878` | 4; 9 | A batch without reads |
| `tempconflictbtree.cpp:1932` | 4 | A leaf emptied by its compaction |
| `tempconflictbtree.cpp:1979`, `1981` | 11; 4 | The second line of a key prefetched, in the next key block |
| `tempconflictbtree.cpp:1993`, `2026` | 5; 5 | A stored key and a read key of length 0 |
| `tempconflictbtree.cpp:2031` | 5 | A read key longer than the rest of its line |
| `tempconflictbtree.cpp:2051` | 8; 10 and 11 never executed | An expired or marked separator at a partition step |
| `tempconflictbtree.cpp:2057`, `2221` | 5; 5 | A separator marked with `EmptyChildCV` |
| `tempconflictbtree.cpp:2133`, `2145` | 5; 5 | `findPrev` moves to the leaf on the left |
| `tempconflictbtree.cpp:2189` | 4 | A leaf with no entries reached |
| `tempconflictbtree.cpp:2384` | 20 | A round runs a task in `FindPrev` (line 2397) |
| `tempconflictbtree.cpp:2506` | 10 | A batch whose first operation is a write |
| `tempconflictbtree.cpp:2515` | 9 | An empty tree |

**Source outcomes not reachable (47 items).** llvm-cov reports these
outcomes as not covered, or, where marked, gcov reports them never
executed. They cannot occur while the tree's documented invariants and the
interface preconditions hold.

| Location | gcov items | Outcome | Why it does not occur |
|---|---|---|---|
| `tempconflictbtree.cpp:59` | 1 | A write of a too-old transaction reaching `addWrites` | `buildBeginIndexes` decides a transaction is too old at its first begin, read or write, and records no endpoint of it |
| `tempconflictbtree.cpp:143` | 1 | `savedLeafIsCurrent` without a saved revision | It is called only after `resetSearch` or `finishInterval` saved one |
| `tempconflictbtree.cpp:271` | 5, 12 | `switch` of `advance` to `case Step::Previous`, and its `default` | `Previous` is set only immediately before `goto previous` (line 392), so no visit starts in it; every value of `Step` has a case |
| `tempconflictbtree.cpp:286` (condition of line 287) | 5 | Saved leaf no longer current after `reserveWrite` | With the same cursor leaf and the same cut no block was reclaimed, and the revision was checked or saved in the same visit |
| `tempconflictbtree.cpp:337` | 2 | `hintKey` returns false for the first live entry of the right neighbour | It returns false only for an empty key, which can only be the first entry of the leftmost leaf |
| `tempconflictbtree.cpp:404` | 1, 5 | Previous entry in the cursor leaf expired, or with no bytes | The head task compacts the cursor leaf with the current cut (lines 308, 348) and no block is reclaimed before the insertion; a stored range has at least one byte |
| `tempconflictbtree.cpp:422`, `461` | 4; 2 | `hintKey` of a whole stored range returns false | A stored range has at least one byte |
| `tempconflictbtree.cpp:231` (no gcov item) | - | `prepareErase` on a leaf with one entry and no neighbours | The scanned leaf is the cursor leaf, which holds the new write and the entry, or lies to its right |
| `tempconflictbtree.cpp:712` | 3 | `redirectWriteSubtree` with a cursor and no prepared destinations | In this build a cursor exists only inside `addWrites`, after `prepareWriteDestinations` (line 39) |
| `tempconflictbtree.cpp:752` | 0 | A buffered write leaf without writes | `bufferTaskLeaf` returns before buffering when the slice has no write (line 2158) |
| `tempconflictbtree.cpp:803` | 3; 804: 0 and 1 never executed | No destination leaf in a non-empty tree (the `std::logic_error`) | A null cursor leaf is replaced by `rightmostLeaf_` (lines 151, 284), which is not null while there is a root |
| `tempconflictbtree.cpp:1039` | 0 | `normalizeRoot` loop ends by its condition | Every caller has a root, and the loop sets the root to null only before returning |
| `tempconflictbtree.cpp:1414` | 2 | Parent still needs a split after losing an empty child | The empty child is not counted as needing a split, so erasing it adds a free slot and leaves the count unchanged; a node whose count equalled its free slots no longer does |
| `tempconflictbtree.cpp:1425` | 0 | `removeIfEmpty` on an empty internal root (lines 1426-1430) | Its callers pass leaves: line 347 and `cleanPending`, whose queue holds only leaves (lines 1311, 1318, 1933) |
| `tempconflictbtree.cpp:1519` | 2 | A leaf split while a cursor on another leaf is active | With a cursor active, a leaf is split only by `prepareWriteSplit` on the cursor's leaf |
| `tempconflictbtree.cpp:1904` | 1, 9 | Read of a too-old transaction during the traversal | `buildBeginIndexes` decides a transaction is too old at its first begin and skips all its entries |
| `tempconflictbtree.cpp:1961` | 1, 2, 9, 10 | Slow prefetch with length 0 or an invalid key | `prefetchStoredKey` returns before for length 0; only live entries and separators are prefetched, separators after being refreshed, and their key blocks have CVs above `reclaimCV`, so they are not reclaimed |
| `tempconflictbtree.cpp:1971` | 0, 4 | Prefetch loop ends by its condition | Line 1979 returns when the second line is done |
| `tempconflictbtree.cpp:2139` | 0, 4 | Expired entry met by `findPrev` | `findPrev` compacts the leaf first, and the cut does not change during `checkReads` |
| `tempconflictbtree.cpp:2160` | 3, 15 | `bufferTaskLeaf` without a leaf | It is called without a leaf only from line 2204, which is not reachable |
| `tempconflictbtree.cpp:2200` | 1, 5; 2202: 0, 1, 4 and 5 never executed | Internal node without children reached (lines 2201-2208) | Only a root leaf may be empty when an operation returns, and `checkReads` removes no child before its traversal ends |
| `tempconflictbtree.cpp:2384` | 0, 16 | `default` of `runRound` (line 2413) | Tasks in the order are never `Empty` or `Done` at the start of a round: `endRound` removes them |
| `tempconflictbtree.cpp:2444` | 1, 9 | Task `Empty` in `endRound` | Set only by line 2205, which is not reachable |
| `tempconflictbtree.cpp:2500` | 1, 5; 2501: 0, 1, 4 and 5 never executed | Too many operations (the `std::length_error`) | There is at most one operation per endpoint, and `buildBeginIndexes` throws for 2^31 endpoints or more |

**Source outcomes reachable but not exercised (44 items).** They follow
from inputs that no test provides. Where an item is never executed, it is
on a line that only that outcome reaches.

| Location | gcov items | Outcome | Input that reaches it |
|---|---|---|---|
| `tempconflictbtree.cpp:278` | 2 | Another interval after an empty one | An empty write range followed by another accepted range in the same destination group |
| `tempconflictbtree.cpp:112`, `134`, `196`, `366` | 1; 0; 0; 2 | A write or a stored entry whose begin is the empty key | A write range that begins at the empty key |
| `tempconflictbtree.cpp:33`, `2505`, `2515` | 0; 1, 5; 3, 11 | A batch with no operation | A batch with no endpoints, or whose reads are all empty, too old or skipped, and no write |
| `tempconflictbtree.cpp:36` | 0 | `addWrites` after a batch with reads only | `addWrites` called after `checkReads` on a batch without writes |
| `tempconflictbtree.cpp:252`, `291` | 0; 1 | A head task's search reset, or its reservation, leaves no cursor leaf | A reservation that reclaims every node of the tree during `addWrites` |
| `tempconflictbtree.cpp:153` (no gcov item), `2160` | -; 1, 5, 13, 17 | A destination that needs a right check | A write routed past a separator marked with `EmptyChildCV`, or to a leaf left empty by its compaction during `checkReads` |
| `tempconflictbtree.cpp:331`, `329` | 1; 334: 0 and 1 never executed | A right neighbour without live entries during a right check | A right check whose right neighbour holds only expired entries |
| `tempconflictbtree.cpp:347` | 0, and 2, 3 never executed | The cursor leaf left empty when the write moves to the right neighbour | A right check from a leaf whose entries all expired |
| `tempconflictbtree.cpp:717` | 0 | A redirected subtree with more than one leaf | Reclamation during `addWrites` of a subtree that holds two or more leaves |
| `tempconflictbtree.cpp:725` | 1; 727, 728, 729: 0 and 1 never executed | The write-leaf buffer grows beyond 2 G entries | More than 2 G buffered write leaves waiting for an earlier slice |
| `tempconflictbtree.cpp:736` | 1; 739: 0 and 1 never executed | A ring position of the write-leaf buffer reused | More buffered write leaves in one batch than the buffer's capacity |
| `tempconflictbtree.cpp:775` (condition of line 776) | 2 | The reserve is not reached but the write fits | A pool of fewer than 35 blocks (one 64 KiB page holds 32): each write then reclaims all history it can and is stored |
| `tempconflictbtree.cpp:1033` | 1 | An entry inserted at or below the cut | A batch whose reservations reclaim blocks written earlier in the same batch, raising the cut to its version |
| `tempconflictbtree.cpp:1827` | 1, 5; 1828: 0, 1, 4 and 5 never executed | Too many endpoints (the `std::length_error`) | A batch of 2^31 endpoints or more |
| `tempconflictbtree.cpp:2090` | 2, 14 | A task resumed in `FindPrev` (lines 2101-2103) | A task waiting in `FindPrev` among those suspended when more than G tasks are active |

On line 2090 the correspondence of the items with the cases comes from
the llvm-cov counts, which report `case Step::FindPrev` of
`prefetchAgain` (line 2101) as the only case never taken; the two cases
that share a body, `InspectNode` and `ScanLeaf`, have one edge.

**Source outcomes whose reachability was not determined (10 items).**
llvm-cov reports these outcomes as not covered, or gcov reports them never
executed. No input producing them was identified, and they were not shown
to be excluded by an invariant.

| Location | gcov items | Outcome | What is known |
|---|---|---|---|
| `tempconflictbtree.cpp:145` | 1, 2 | A saved leaf whose block was released (even generation) or reused (another generation) | The tests produce saved-leaf changes only through the mutation epoch (line 146), and an earlier condition of lines 245-247 detects each reclamation of a saved leaf that they produce |
| `tempconflictbtree.cpp:304` (conditions of lines 304 and 305) | 3, 5 | A task that is not the head parks on a leaf because the cut changed since leaves were compacted, or because its cursor requires a right check | Each reclamation in `addWrites` is followed, in the same visit, by the head task's `cleanExpired`, which records the new cut; right checks are not exercised (see line 2160 above) |
| `tempconflictbtree.cpp:415` | 1; 417: 0 and 1 never executed | An empty leaf on the left while looking for a previous entry | Leaves left empty by `checkReads` are removed by `cleanPending` before `addWrites` |
| `tempconflictbtree.cpp:652` | 0; 656: 0 and 1 never executed | Two write leaves with the same destination | The partition of `checkReads` gives each leaf at most one slice in the tested batches |

## Limitations

- Mixed batches are tested by two cases on one history shape, with five
  and three transactions; larger random mixed batches are not tested
  against a reference.
- Other paths reachable through the interface are not exercised: an empty
  write range followed by another range of the same destination, a write
  that begins at the empty key, a batch with no operation, destinations
  that need a right check, growth and reuse of the write-leaf buffer,
  reclamation during `addWrites` that empties the tree, removes a subtree
  of several leaves or raises the cut to the batch's version, a pool
  smaller than the 35-block reserve, a batch of 2^31 endpoints or more,
  and the resumption of a task suspended in `FindPrev`. See
  [Uncovered generated branches](#uncovered-generated-branches). Ten
  generated branches have an undetermined reachability; they are listed
  with what is known.
- `checkReads<24>` and the write path of the `RESOLVER1_WRITE_LG` 0 build
  are compiled but not run by the tests.
- No allocation-failure or exception injection was performed. The effect
  of a `std::bad_alloc`, of the `std::overflow_error` of an exhausted
  generation in `KeyStore` or `BNodeStore`, or of the `std::length_error`
  of an oversized batch in `addWrites` or `checkReads`, and the state of
  the tree after it, are not tested.
- `checkReads` is tested with one read version per transaction, as
  `ConflictBatch` builds it; reads of one transaction with different
  versions were not tested.
- Coverage was measured only with 2048-byte blocks and the default
  `RESOLVER1_WRITE_LG` and `RESOLVER1_WRITE_GROUPS`; the makefile can
  build other block sizes with `BLOCK_SIZE`.
- The prefetch hints are not validated beyond their execution: their
  effect on performance is outside the scope of this component.
- No condition coverage was measured: the toolchain does not provide it.
  The table of [Independent condition checks](#independent-condition-checks)
  names, for each outcome, a test that produces it; it does not show that
  each condition alone decides its decision.
- Release-style `-O2 -DNDEBUG` builds were tested with LG 8×2 and LG 4×2,
  using block sizes of 1, 2, 4 and 8 KiB. All eight configurations passed
  232 of 232 tests. The runs are recorded in `matrix/summary.txt` (SHA-256
  `073fa663a89a839eeebb92c7bdf587030618c3d9be76d7ed09aec3a851a7dd4f`) of the
  external package
  `mac-to-hetzner/2026-11-08-v3-mac-mixed-batch-tests-and-final-catalogs`
  (by5-exchange commit `b0778c0365`).
- Concurrent access, and the rule that no reclamation occurs while key
  bytes are accessed, are not tested.
- The reserve policy after a long key was measured only with the tree
  driven directly (see [Reserve after a long key](#reserve-after-a-long-key)),
  not on an FDB workload.
- Results apply to the source and environment identified above.

## Evidence

- [Test output](evidence/tempconflictbtree-tests.txt): output of
  `make -B test` for the full suite (232 tests: PagePool, KeyStore,
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
