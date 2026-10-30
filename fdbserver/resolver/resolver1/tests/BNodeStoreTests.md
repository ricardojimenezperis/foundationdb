# BNodeStore validation

## Scope

Validation of B+ tree node storage in blocks taken from a shared
`PagePool`: the node capacity chosen for the block size, the map of free
slots of a node (`FreeSlots<C>`, one 64-bit word per 64 slots), the
allocation and initialization of leaf and internal nodes, whether a
node has free slots, increasing generations, the age list kept in
allocation and commit-version order, release of nodes to the pool and
their reuse, repositioning of a node in the age list when its commit
version grows, the oldest node and its commit version, the return of
all remaining blocks when the store is destroyed, and, within one node,
insertion and erasure of entries at a given rank through the node's
`order` array, reuse of free slots, the raise of a leaf's commit version
by a newer entry, the parent and rank of a child in its parent, and
binary search of an external key (`StringRef`) without storing it. Leaf
entries are key ranges (`RangeRef`), searched by their begin with
`compareBegin`; internal entries are separator keys, held as the
`RangeRef` of a single key that `KeyStore::appendKey` returns and
searched with `compareKeys`.

The block size is chosen when compiling, with
`-DRESOLVER1_BLOCK_SIZE=<bytes>` (`make BLOCK_SIZE=<bytes>`), and is
2048 bytes by default. The tests were built and run with the default,
for which `InternalCapacity` is 40 and `LeafCapacity` is 59.

Outside the scope of this component: `PagePool` itself (see
[PagePoolTests.md](PagePoolTests.md)), key storage and key comparison
(see [KeyStoreTests.md](KeyStoreTests.md)), tree navigation, splits,
separators chosen for them, parent-child and leaf links, which are
maintained by the caller, the choice of the commit-version cutoff for
reclamation, and the use that the tree makes of node fields that only
it reads (`childrenNeedingSplit`, `lastCleanReadBatch`, and the
generation that `releaseNode` clears), which belongs to the tree (see
[TempConflictBTreeTests.md](TempConflictBTreeTests.md)).
`tempconflictbtree.cpp` is linked into the same test binary and
instantiates `FreeSlots` for capacities of its own; the BNodeStore
tests do not call it. `fdbcompat.h` provides stand-ins for FoundationDB
types (`StringRef`) so that the component builds without `flow`, and
includes `../ResolverBatchTypes.h`; neither is measured as part of this
component.

## Source and environment

- Source commit: the tested files are identified by content; this report
  is committed together with them. SHA-256 (first 16 hex digits):

  | File | SHA-256 |
  |---|---|
  | `bnodestore.cpp` | `a945a23136d881e9` |
  | `bnodestore.h` | `4ff857dd23b118d6` |
  | `keystore.cpp` | `b09886264fe0237c` |
  | `keystore.h` | `5669791aadb07001` |
  | `fdbcompat.h` | `2e57a5fd8b1629ea` |
  | `../ResolverBatchTypes.h` | `44e93cb91d982c40` |
  | `common.h` | `a815af89662b3a73` |
  | `pagepool.cpp` | `1b463f87266e646f` |
  | `pagepool.h` | `54456873974dbb8f` |
  | `test.h` | `a9a025cd287fd82e` |
  | `testmain.cpp` | `f977afc8cc420126` |
  | `makefile` | `2a8173902bc33339` |
  | `tests/bnodestoretests.cpp` | `eba26821ec6756c6` |

- Working tree: the files listed above
- Platform: macOS 27.0.1 (build 26A434), arm64 (Apple M3 Pro)
- Compiler: Apple clang 15.0.0 (clang-1500.3.9.4), target
  `arm64-apple-darwin27.0.0`
- Language standard: C++20
- Build flags: `-I. -std=c++20 -Wall -Wextra -Wpedantic -g`, without
  `-DRESOLVER1_BLOCK_SIZE`, so `BlockSize` is 2048
- Coverage tools and versions:
  - `gcov` (Apple LLVM version 15.0.0, `llvm-cov gcov` compatible mode)
  - `llvm-cov` and `llvm-profdata` (Apple LLVM version 15.0.0, via `xcrun`)
- Coverage build flags:
  - gcov: `-I. -std=c++20 -Wall -Wextra -Wpedantic -O0 --coverage`
  - llvm-cov: `-I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=.`

## Reproduction

Run from the project root:

```bash
make test
```

To run only this component's tests:

```bash
./tests.exe /resolver/BNodeStore
```

Coverage collection with gcov:

```bash
rm -rf build/coverage && mkdir -p build/coverage
FLAGS="-I. -std=c++20 -Wall -Wextra -Wpedantic -O0 --coverage"
for f in pagepool keystore bnodestore tempconflictbtree testmain tests/pagepooltests tests/keystoretests tests/bnodestoretests tests/tempconflictbtreetests; do
	g++ $FLAGS -c $f.cpp -o build/coverage/$(basename $f).o
done
g++ --coverage build/coverage/*.o -o build/coverage/tests.exe
(cd build/coverage && ./tests.exe /resolver/BNodeStore)
gcov -b -c -o build/coverage/bnodestore.o bnodestore.cpp > build/coverage/bnodestore-coverage.txt 2>&1
mv *.gcov build/coverage/
```

Coverage collection with llvm-cov:

```bash
rm -rf build/coverage-llvm && mkdir -p build/coverage-llvm
g++ -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=. pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp testmain.cpp tests/*.cpp -o build/coverage-llvm/tests.exe
LLVM_PROFILE_FILE=build/coverage-llvm/bnodestore.profraw build/coverage-llvm/tests.exe /resolver/BNodeStore
xcrun llvm-profdata merge -o build/coverage-llvm/bnodestore.profdata build/coverage-llvm/bnodestore.profraw
xcrun llvm-cov report build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/bnodestore.profdata '-ignore-filename-regex=(pagepool|^keystore|tempconflictbtree|fdbcompat|ResolverBatchTypes|test)' > build/coverage-llvm/bnodestore-llvm-report.txt
xcrun llvm-cov show build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/bnodestore.profdata -show-branches=count '-ignore-filename-regex=(pagepool|^keystore|tempconflictbtree|fdbcompat|ResolverBatchTypes|test)' > build/coverage-llvm/bnodestore-llvm-show.txt
```

Only the BNodeStore tests are run. Each collection starts from an empty
directory, so counters from earlier runs are not merged in. The
`-ignore-filename-regex` option leaves only `bnodestore.cpp` and
`bnodestore.h` in the llvm-cov output; it removes `fdbcompat.h` and
`ResolverBatchTypes.h`, which the store includes through `keystore.h`.

`tests/coverage.sh` runs these commands for the four components, after
the full suite whose output is the test evidence. Run it from the
project root:

```bash
bash tests/coverage.sh
```

The exact branch counts summed over template instantiations, given in
[Decisions](#decisions), were read from the same profile with:

```bash
xcrun llvm-cov export build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/bnodestore.profdata '-ignore-filename-regex=(pagepool|^keystore|tempconflictbtree|fdbcompat|ResolverBatchTypes|test)'
```

## Test cases

All tests use pools of `BlockSize` blocks, the block size that the
store requires, and a `KeyStore` over the same pool, declared before the
`BNodeStore` so that it outlives it. Pools for full nodes are sized from
`InternalCapacity`, `LeafCapacity` and `BlockSize`, with at least 4
pages.

Capacity, free-slot maps and node lifecycle:

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/BNodeStore/CapacityForBlock` | Called at run time, `capacityForBlock` returns `InternalCapacity` and `LeafCapacity`; each is the largest capacity whose layout fits in `BlockSize`, since one more entry does not fit. A layout of one byte per entry is capped at `MaxNodeCapacity` (255), and a layout that does not fit even one entry gives 0. | PASS |
| `/resolver/BNodeStore/FreeSlots` | For capacities 1, 40, 59, 64, 65, 121, 128, 165, 245 and 255, a map has `(C + 63) / 64` words; a new map has exactly the `C` lowest bits set, word by word, and `fullWord(w)` equals each word; `any` is true and `count` is `C`. Taking the slots in order, `first` returns each one, `isFree` turns false and `count` goes down by one, until `any` is false and every word is 0. Unsetting a used slot, and setting a free slot twice, change nothing. For `C > 1`, `first` returns the lowest free slot, also when the lower words are empty. | PASS |
| `/resolver/BNodeStore/FreeSlotsSize` | `FreeSlots<64>` is one `std::uint64_t`, `FreeSlots<65>` two and `FreeSlots<255>` four. | PASS |
| `/resolver/BNodeStore/AllocateLeafInitializesNode` | A new leaf has kind `Leaf`, generation 1, the given `maxCV`, no entries, no parent and parent slot 0, no age links, a free-slot map equal to a new `FreeSlots<LeafCapacity>`, no left or right neighbour and a zeroed order. | PASS |
| `/resolver/BNodeStore/AllocateInternalInitializesNode` | A new internal node has kind `Internal`, generation 1, the given `maxCV`, no entries, no parent and parent slot 0, no age links, a free-slot map equal to a new `FreeSlots<InternalCapacity>` and a zeroed order. | PASS |
| `/resolver/BNodeStore/AllocatedNodesHaveAllSlotsFree` | A new leaf and a new internal node report `freeSlotCount` equal to their capacity and `firstFree` 0. | PASS |
| `/resolver/BNodeStore/GenerationsIncreaseAcrossKinds` | Leaves and internal nodes share one generation sequence: 1, 2, 3. | PASS |
| `/resolver/BNodeStore/AgeListFollowsAllocationOrder` | Three nodes of both kinds are linked in allocation order through `agePrevious` and `ageNext`, with no link before the first or after the last. | PASS |
| `/resolver/BNodeStore/AllocationFailsWhenPoolExhausted` | With every block of a one-page pool used, allocating a leaf or an internal node returns `nullptr`, consumes no generation and leaves the age list unchanged; after a release, the next node gets the next generation and follows the newest remaining node. | PASS |
| `/resolver/BNodeStore/ReleaseReturnsBlockToPool` | Releasing a leaf returns its block to the pool; the same address is reused for an internal node, initialized as such, with every slot free and with a generation higher than the leaf's, read before the release; releasing it restores the free-block count. | PASS |
| `/resolver/BNodeStore/ReleaseOnlyNode` | After releasing the only node, the age list is empty: the next node has no predecessor. | PASS |
| `/resolver/BNodeStore/ReleaseOldestNode` | Releasing the oldest of three nodes leaves the other two linked, the second one first. | PASS |
| `/resolver/BNodeStore/ReleaseMiddleNode` | Releasing the middle node links its neighbours to each other. | PASS |
| `/resolver/BNodeStore/ReleaseNewestNode` | Releasing the newest node makes its predecessor the newest: it has no successor, and the next node follows it. | PASS |
| `/resolver/BNodeStore/UpdateCVSameValueKeepsPosition` | Updating a node to its current `maxCV` leaves it in place. | PASS |
| `/resolver/BNodeStore/UpdateCVNewestNodeStaysNewest` | Raising the `maxCV` of the newest node updates the value and leaves the list unchanged. | PASS |
| `/resolver/BNodeStore/UpdateCVMovesNodeToNewest` | Raising the `maxCV` of the oldest node, and then of a middle node, moves each to the end of the age list; the next node follows the moved one; generations keep their allocation order. | PASS |
| `/resolver/BNodeStore/OldestCV` | `oldestCV` is empty for an empty store, returns the `maxCV` of the oldest node, follows the oldest node when it is moved by `updateCV` or released, and is empty again when every node is released. | PASS |
| `/resolver/BNodeStore/OldestNode` | `oldestNode` is `nullptr` for an empty store, returns the address of the oldest node, follows the oldest node when it is moved by `updateCV` or released, and is `nullptr` again when every node is released. | PASS |
| `/resolver/BNodeStore/DestructorReturnsAllBlocks` | Destroying a store with leaves and internal nodes, one of them moved by `updateCV`, returns every block to the pool. | PASS |
| `/resolver/BNodeStore/HasSpace` | `hasSpace` is true for a new leaf and a new internal node, still true with a single free slot (the lowest for the leaf, the highest for the internal node), and false with no free slots. | PASS |
| `/resolver/BNodeStore/BlockOverloads` | `updateCV` on a `LeafBlock*` and on an `InternalBlock*` raises the node's `maxCV` and moves it to the newest end, and `releaseNode` on each kind removes it from the age list; the free-block count is restored. | PASS |

Entries within a node. Every insertion and erasure test checks the node
after each operation against a reference sequence kept in a
`std::vector`: `count`, the range and commit version (leaves) or the
separator, separator commit version and child (internal nodes) at each
rank through `order`, that no slot appears twice in `order`,
that `freeSlots` holds exactly the unused slots, and `hasSpace`. In
internal nodes it also checks that every child still in the node points
to it as `parent`, holds its slot as `parentSlot` and its rank as
`parentRank()`, and that `maxSubtreesCV` of the slot equals the child's
`maxCV`. Leaf entries alternate point ranges and general ranges, so
that leaves hold both kinds of record.

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/BNodeStore/InsertAtEnd` | An empty internal node is filled to `InternalCapacity`, always inserting at the end. | PASS |
| `/resolver/BNodeStore/InsertAtBeginning` | The same, always inserting at the beginning. | PASS |
| `/resolver/BNodeStore/InsertInMiddle` | The same, always inserting at the middle rank. | PASS |
| `/resolver/BNodeStore/InsertAtMixedPositions` | The same, alternating the beginning, the end, the middle, the second rank and the last rank. | PASS |
| `/resolver/BNodeStore/InsertAtEndOfLeaf` | An empty leaf is filled to `LeafCapacity`, always inserting at the end. | PASS |
| `/resolver/BNodeStore/InsertAtBeginningOfLeaf` | The same, always inserting at the beginning. | PASS |
| `/resolver/BNodeStore/InsertInMiddleOfLeaf` | The same, always inserting at the middle rank. | PASS |
| `/resolver/BNodeStore/InsertAtMixedPositionsOfLeaf` | The same, alternating the five positions above. | PASS |
| `/resolver/BNodeStore/EraseAtEnd` | A full internal node is emptied, always erasing the last entry; the last erasure removes the only entry and frees every slot. | PASS |
| `/resolver/BNodeStore/EraseAtBeginning` | The same, always erasing the first entry. | PASS |
| `/resolver/BNodeStore/EraseInMiddle` | The same, always erasing the middle entry. | PASS |
| `/resolver/BNodeStore/EraseAtMixedPositions` | The same, alternating the first, the last, the middle, the second and the second-to-last entry. | PASS |
| `/resolver/BNodeStore/EraseAtEndOfLeaf` | A full leaf is emptied, always erasing the last entry. | PASS |
| `/resolver/BNodeStore/EraseAtBeginningOfLeaf` | The same, always erasing the first entry. | PASS |
| `/resolver/BNodeStore/EraseInMiddleOfLeaf` | The same, always erasing the middle entry. | PASS |
| `/resolver/BNodeStore/EraseAtMixedPositionsOfLeaf` | The same, alternating the five positions above. | PASS |
| `/resolver/BNodeStore/EraseThenInsertReusesSlot` | In a full internal node, erasing a middle entry frees exactly its slot, and inserting at another rank reuses that slot and fills the node again. | PASS |
| `/resolver/BNodeStore/EraseThenInsertReusesSlotInLeaf` | The same in a full leaf. | PASS |
| `/resolver/BNodeStore/InsertAndEraseInterleaved` | 300 pseudo-random insertions and erasures in an internal node, with a fixed seed; every insertion takes the lowest free slot. The sequence reaches the full node 53 times and the empty node once. | PASS |
| `/resolver/BNodeStore/InsertAndEraseInterleavedInLeaf` | The same in a leaf, with another seed; the sequence reaches the full leaf 45 times and the empty leaf 5 times. | PASS |
| `/resolver/BNodeStore/InsertAtRaisesLeafCV` | Inserting entries with commit versions 5 and 10 into a leaf with `maxCV` 10 leaves its `maxCV` and the age list unchanged; inserting one with 30 raises the leaf's `maxCV` to 30 and moves it to the newest end, so the other leaf, with 20, becomes the oldest. | PASS |
| `/resolver/BNodeStore/ParentAndParentRank` | `parent()` of a leaf and of an internal node is `nullptr` before insertion and the parent block after it. `parentRank()` follows the logical rank, not the slot: a second child inserted at rank 0, in slot 1, has rank 0 and moves the first child, in slot 0, to rank 1 (also through `BNodeHeader::parentRank`); erasing rank 0 moves it back to rank 0. | PASS |
| `/resolver/BNodeStore/FindKeyInEmptyNodes` | In an empty leaf and an empty internal node, searching an external key returns false and rank 0. | PASS |
| `/resolver/BNodeStore/FindKeyInLeaf` | Ten ranges, alternately points and general ranges, with begins at the ten search keys, inserted in the order 5, 2, 8, 0, 9, 3, 7, 1, 6, 4 of their sorted position, so that `order` is not the identity; the search compares with each range's begin. Each key, searched as an external `StringRef` over a local array and not stored, is found with its rank. Absent keys before the first, between each pair and after the last return false and the rank where they would be inserted, as do a proper prefix of every key, a key longer than an existing one, and the empty key. | PASS |
| `/resolver/BNodeStore/FindKeyInInternalNode` | The same in an internal node, whose separators are single keys. | PASS |

The counts of full and empty nodes reached by the interleaved tests come
from replaying their random decisions with the same seeds and the
capacities above, not from the test output.

47 BNodeStore tests passed, 0 failed. The full run, including the 15
PagePool tests, the 41 KeyStore tests and the 124 TempConflictBTree
tests, reports 228 passed and 0 failed.

The store has these preconditions, checked with `assert`, which
terminates the process when it fails and is removed when `NDEBUG` is
defined. The tested builds do not define `NDEBUG`. Inputs that do not
meet them are outside the interface and are not tested.

- The pool block size equals `BlockSize`.
- A node is allocated with a commit version no lower than the `maxCV`
  of the newest node.
- `releaseNode` and `updateCV` receive a non-null node of this store.
  `releaseNode` also requires that the node has been detached from the
  tree.
- `updateCV` never lowers a node's `maxCV`, and a raised value is no
  lower than the `maxCV` of the newest node. `insertAt` on a leaf calls
  `updateCV` when the entry's commit version is above the leaf's
  `maxCV`, so such an entry has the same requirement.
- `insertAt` receives a non-null node with a free slot and a rank no
  greater than its entry count; for an internal node, also a non-null
  child.
- `eraseAt` receives a non-null node and a rank lower than its entry
  count.
- `findKey` on a leaf receives a non-null node.
- `FreeSlots::first`, and `firstFree` of both node kinds, require a free
  slot.
- `parentRank` requires a parent, and the child's slot among the first
  `count` ranks of the parent's `order`.

These preconditions are part of the interface but are not checked:

- `findKey` requires the entries of the node to be in key order through
  `order`; `insertAt` stores an entry at the rank it is given and does
  not compare keys. The tests insert keys at their sorted rank.
- `eraseAt` on an internal node leaves the removed child unchanged,
  including its `parent` and `parentSlot`. Relinking or releasing that
  child is the caller's responsibility, because splits and erasures
  need different handling.
- `isFree`, `setFree` and `unsetFree` of `FreeSlots<C>` take a slot
  lower than `C`; this is implied by the map's size rather than stated
  in the header.

## Coverage

Coverage was collected running only the BNodeStore tests.

Most of the store is defined in `bnodestore.h` as `inline` functions
and templates; `bnodestore.cpp` holds only the constructor and the
destructor. With `--coverage`, each translation unit keeps its own
counters for an inline function and the linker keeps one copy, so gcov
is used only for `bnodestore.cpp`. llvm-cov merges the counters of all
copies and is used for `bnodestore.h`.

gcov, `bnodestore.cpp`:

| Metric | Result |
|---|---|
| Lines executed | 100.00% of 13 |
| Branches executed | 100.00% of 6 |
| Taken at least once | 66.67% of 6 |

llvm-cov source-based coverage:

| File | Regions | Functions | Lines | Branches |
|---|---|---|---|---|
| `bnodestore.cpp` | 100.00% of 4 | 100.00% of 2 | 100.00% of 10 | 100.00% of 2 |
| `bnodestore.h` | 94.52% of 146 | 100.00% of 42 | 100.00% of 303 | 100.00% of 44 |

These are gcov and llvm-cov metrics. They are not a measurement of
MC/DC, and full coverage does not show that the code is free of
errors. "Taken at least once" does not mean that every decision was
taken in both directions. Apple clang 15 provides no condition
coverage equivalent to GCC's `-fcondition-coverage`.

gcov counts branches between the basic blocks of the generated code,
including the failure paths of the `assert` checks and the exception
edges the compiler adds. llvm-cov counts the true and false outcomes of
each condition written in the source outside the `assert` checks, whose
conditions come from a macro in a system header, and reports as
"Folded - Ignored", without counting them, the conditions that are
constant in a template instantiation (`if constexpr` and `C % 64 == 0`).
Its 46 branches are the 23 remaining conditions of the store, 1 in
`bnodestore.cpp` and 22 in `bnodestore.h`, each taken in both
directions.

The 42 functions in `bnodestore.h` are the 8 members of the template
`FreeSlots` (`fullWord`, the constructor, `isFree`, `setFree`,
`unsetFree`, `any`, `count` and `first`), the 7 members of each node
layout (`parent`, `parentRank`, `hasSpace`, `freeSlotCount`,
`firstFree`, `setFree` and `unsetFree`), the template
`capacityForBlock`, `BNodeHeader::parentRank`, and the 18 inline member
functions of `BNodeStore`, counting each overload. llvm-cov counts each
template function once, whatever its number of instantiations. The
`FreeSlots` members instantiated by `tempconflictbtree.cpp` for its own
capacities appear in `bnodestore-llvm-show.txt` as unexecuted
instantiations, because only the BNodeStore tests are run.

llvm-cov reports 8 of the 146 regions of `bnodestore.h` as not covered.
Each starts at the opening brace of a branch of an `if constexpr` that
is not the last one: in the six `FreeSlots` members that choose between
one word and several (`isFree`, `setFree`, `unsetFree`, `any`, `count`
and `first`, lines 84, 91, 98, 105, 117 and 130), and in
`capacityForBlock` (lines 227 and 229). In an instantiation that
discards such a branch, llvm-cov still maps a region for it, which has
no code and is never executed, while an instantiation that keeps it has
no region for the branches it discards. For a template, llvm-cov counts
the largest number of regions of any of its instantiations and the
largest number covered in any of them, so each of these regions is
reported as not covered although every branch is executed by some
instantiation: the single-word branch of the six members by
`FreeSlots<1>`, `<40>`, `<59>` and `<64>`, and the multi-word branch by
`FreeSlots<65>`, `<121>`, `<128>`, `<165>`, `<245>` and `<255>`.
`InternalCapacity` and `LeafCapacity` evaluate `capacityForBlock` at
compile time, so `CapacityForBlock` calls it at run time to execute it,
including on two layouts defined in the test that reach the cap and the
result 0. The tests check the value returned through each branch:
`C - 1` at `InternalBlockLayout<41>` and `LeafBlockLayout<60>`, which
gives 40 and 59 through the recursion, and at the oversized layout with
`C = 1`, which gives 0; and 255 through the cap, at the one-byte layout
with `C = 255`.

### Decisions

The decisions of the store outside the `assert` checks have a single
condition, except the choice of a word in `FreeSlots::fullWord`.

`FreeSlots<C>::fullWord(w)`: `w + 1 < Words || C % 64 == 0` (line 73)
selects a word with every bit set, or with only the `C % 64` lowest bits
set. `C % 64 == 0` is a constant in each instantiation, so llvm-cov
folds it; its two values are covered by different instantiations. The
table assumes 64-bit words, as `FreeSlots` defines them, and does not
depend on the platform.

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| Not the last word | `C = 65`, `w = 0` (and the lower words of 121, 128, 165, 245 and 255) | Not evaluated | Every bit set | `FreeSlots` |
| Last word, `C` a multiple of 64 | `C = 64`, `w = 0`; `C = 128`, `w = 1` | `w + 1 < Words` false | Every bit set | `FreeSlots` |
| Last word, `C` not a multiple of 64 | `C = 65`, `w = 1` (and 1, 40, 59, 121, 165, 245 and 255) | `w + 1 < Words` false | `C % 64` lowest bits set | `FreeSlots`; every node allocation (40 and 59) |

llvm-cov reports both outcomes covered for every counted condition.
`llvm-cov show` lists the branches of a template once per
instantiation; the counts below are their sums, from `llvm-cov export`
on the same profile (see [Reproduction](#reproduction)):

| Location | Condition | True | False |
|---|---|---|---|
| `bnodestore.cpp:11` | Destructor: nodes remain | 629 | 44 |
| `bnodestore.h:73` | `fullWord`: not the last word | 22 | 2089 |
| `bnodestore.h:78` | `FreeSlots` constructor: words left | 2090 | 2079 |
| `bnodestore.h:108` | `any`, several words: words left | 2054 | 12 |
| `bnodestore.h:109` | `any`, several words: word not empty | 1009 | 1045 |
| `bnodestore.h:121` | `count`, several words: words left | 3174 | 997 |
| `bnodestore.h:134` | `first`, several words: word empty | 1011 | 1003 |
| `bnodestore.h:252` | `parentRank`: slot not at this rank | 268658 | 16740 |
| `bnodestore.h:353` | No block for a leaf | 1 | 616 |
| `bnodestore.h:365` | No block for an internal node | 1 | 26 |
| `bnodestore.h:377` | Empty age list on insertion | 46 | 604 |
| `bnodestore.h:386` | Unlinked node is the newest | 8 | 13 |
| `bnodestore.h:391` | Unlinked node is the oldest | 17 | 4 |
| `bnodestore.h:408` | `updateCV` with the current value | 1 | 720 |
| `bnodestore.h:413` | Updated node is not the newest | 8 | 712 |
| `bnodestore.h:420` | `oldestCV` on an empty store | 2 | 4 |
| `bnodestore.h:470` | `insertAt`, leaf: entry newer than the leaf | 712 | 12 |
| `bnodestore.h:482` | `findKey`, internal node: range not empty | 80 | 15 |
| `bnodestore.h:484` | `findKey`, internal node: key found | 10 | 70 |
| `bnodestore.h:488` | `findKey`, internal node: key before the middle entry | 45 | 25 |
| `bnodestore.h:508` | `findKey`, leaf: range not empty | 80 | 15 |
| `bnodestore.h:510` | `findKey`, leaf: key found | 10 | 70 |
| `bnodestore.h:514` | `findKey`, leaf: key before the middle entry | 45 | 25 |

The `unlinkNode` conditions on lines 386 and 391 are covered by
releasing the only, oldest, middle and newest node, and by moving the
oldest and a middle node in `updateCV`. The leaf entry that is not newer
than its leaf, line 470 false, comes from `InsertAtRaisesLeafCV` and
`FindKeyInLeaf`. The internal-node `insertAt` and both `eraseAt` have no
conditions outside their `assert` checks; llvm-cov reports every one of
their lines executed.

### Uncovered generated branches

gcov, which covers only `bnodestore.cpp`, reports 2 untaken branches.
Their origin was checked in the LLVM IR that Apple clang 15 generates
for `bnodestore.cpp` at `-O0`, with and without `--coverage`.

**Precondition violation.** The path to `__assert_rtn` taken when the
`assert` in the constructor fails, which happens only when the pool
block size differs from `BlockSize`.

| Location | gcov item |
|---|---|
| `bnodestore.cpp:7` | branch 0 |

**Exception path not exercised.** The exception edge of the call to
`releaseBlock` in the destructor (`invoke` instruction). The destructor
is implicitly `noexcept`, so the edge leads to `__clang_call_terminate`.
`releaseBlock` is not declared `noexcept`; its current implementation
contains no operation that throws, but no fault injection was
performed.

| Location | gcov item | Call |
|---|---|---|
| `bnodestore.cpp:14` | branch 1 | `releaseBlock` |

The failure paths of the `assert` checks in `bnodestore.h` are not
measured: llvm-cov does not count them and gcov is not used for the
header.

## Limitations

- Only the default block size, 2048 bytes, was tested. The capacities,
  the counts of full and empty nodes in the interleaved tests and the
  coverage figures above depend on it.
- The store has no accessor for the newest node. The age list is
  observed through the `agePrevious` and `ageNext` links of each node,
  and the newest node through the predecessor of the next allocated
  node.
- The BNodeStore tests check the fields that the store sets at
  allocation, but not the initial values of `childrenNeedingSplit` and
  `lastCleanReadBatch`, which only the tree uses, nor the generation
  that `releaseNode` clears; the lines that set them are executed.
- No fault injection was performed for the exception edge of
  `releaseBlock` in the destructor. Its current implementation contains
  no throwing operation; a change in `PagePool` would make this path
  relevant.
- The failure paths of the `assert` checks in `bnodestore.h` are not
  measured by either tool.
- A build with `NDEBUG` defined removes the precondition checks; such a
  build was not tested.
- No condition coverage was measured: the toolchain does not provide
  it.
- Results apply to the source and environment identified above.

## Evidence

- [Test output](evidence/bnodestore-tests.txt): output of `make -B test`
  for the full suite (228 tests, PagePool, KeyStore, BNodeStore and
  TempConflictBTree). It is the same run as `pagepool-tests.txt`,
  `keystore-tests.txt` and `tempconflictbtree-tests.txt`.
- [Coverage summary](evidence/bnodestore-coverage.txt): gcov output
  after running only `./tests.exe /resolver/BNodeStore`. The filter
  does not appear in the gcov output; it is recorded in
  [Reproduction](#reproduction).
- Annotated coverage, `evidence/bnodestore.cpp.gcov`: from the same gcov
  run.
- [llvm-cov summary](evidence/bnodestore-llvm-report.txt): llvm-cov
  report after running only
  `build/coverage-llvm/tests.exe /resolver/BNodeStore`. The filter does
  not appear in the output.
- llvm-cov annotated coverage, `evidence/bnodestore-llvm-show.txt`:
  `bnodestore.cpp` and `bnodestore.h` with line and branch counts, from
  the same llvm-cov run.

The two annotated reports are generated by `tests/coverage.sh` and are not
tracked; running it on the source identified above regenerates the ones
this report describes.
