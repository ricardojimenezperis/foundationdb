# BNodeStore validation

## Scope

Validation of B+ tree node storage in blocks taken from a shared
`PagePool`: the node capacity chosen for the block size, allocation and
initialization of leaf and internal nodes, whether a node has free
slots, increasing generations, the age list kept in allocation and
commit-version order, release of nodes to the pool and their reuse,
repositioning of a node in the age list when its commit version grows,
the oldest node and its commit version, the return of all remaining
blocks when the store is destroyed, and, within one node, insertion and
erasure of entries at a given rank, reuse of free slots and binary
search of an external key (`StringRef`) without storing it. Leaf entries
are key ranges (`RangeRef`), searched by their begin; internal entries
are single separator keys (`KeyRef`).

Outside the scope of this component: `PagePool` itself (see
[PagePoolTests.md](PagePoolTests.md)), key storage and key comparison
(see [KeyStoreTests.md](KeyStoreTests.md)), tree navigation, splits,
separators chosen for them, parent-child and leaf links, which are
maintained by the caller, and the choice of the commit-version cutoff
for reclamation. `tempconflictbtree.h` and `tempconflictbtree.cpp` are in
the project directory but no tested source includes them. `fdbcompat.h`
provides stand-ins for FoundationDB types (`StringRef`) so that the
component builds without `flow`; it is not measured as part of this
component.

## Source and environment

- Source commit: not under version control; tested files identified by
  SHA-256 (first 16 hex digits):

  | File | SHA-256 |
  |---|---|
  | `bnodestore.cpp` | `a945a23136d881e9` |
  | `bnodestore.h` | `2a14f18942ce0544` |
  | `keystore.cpp` | `051ebb1d9c0f25c3` |
  | `keystore.h` | `6325656bc8268aa1` |
  | `fdbcompat.h` | `788cc62b6648bf6d` |
  | `common.h` | `0a0b864acb86c330` |
  | `pagepool.cpp` | `7506a3e1f65b263a` |
  | `pagepool.h` | `b6549ed8ffe91ea8` |
  | `test.h` | `a9a025cd287fd82e` |
  | `testmain.cpp` | `f977afc8cc420126` |
  | `makefile` | `246b6c0baf2b6a99` |
  | `tests/bnodestoretests.cpp` | `8e18f7f4ce07e136` |

- Working tree: the files listed above
- Platform: macOS 27.0.1 (build 26A434), arm64 (Apple M3 Pro)
- Compiler: Apple clang 15.0.0 (clang-1500.3.9.4), target
  `arm64-apple-darwin27.0.0`
- Language standard: C++20
- Build flags: `-I. -std=c++20 -Wall -Wextra -Wpedantic -g`
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
for f in pagepool keystore bnodestore testmain tests/pagepooltests tests/keystoretests tests/bnodestoretests; do
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
g++ -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=. pagepool.cpp keystore.cpp bnodestore.cpp testmain.cpp tests/*.cpp -o build/coverage-llvm/tests.exe
LLVM_PROFILE_FILE=build/coverage-llvm/bnodestore.profraw build/coverage-llvm/tests.exe /resolver/BNodeStore
xcrun llvm-profdata merge -o build/coverage-llvm/bnodestore.profdata build/coverage-llvm/bnodestore.profraw
xcrun llvm-cov report build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/bnodestore.profdata '-ignore-filename-regex=(pagepool|^keystore|fdbcompat|test)' > build/coverage-llvm/bnodestore-llvm-report.txt
xcrun llvm-cov show build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/bnodestore.profdata -show-branches=count '-ignore-filename-regex=(pagepool|^keystore|fdbcompat|test)' > build/coverage-llvm/bnodestore-llvm-show.txt
```

Only the BNodeStore tests are run. Each collection starts from an empty
directory, so counters from earlier runs are not merged in. The
`-ignore-filename-regex` option leaves only `bnodestore.cpp` and
`bnodestore.h` in the llvm-cov output; it removes `fdbcompat.h`, which
the store includes through `keystore.h`.

`tests/coverage.sh` runs these commands for the three components, after
the full suite whose output is the test evidence. Run it from the
project root:

```bash
bash tests/coverage.sh
```

## Test cases

All tests use pools of 2048-byte blocks, the `BlockSize` that the store
requires, and a `KeyStore` over the same pool, declared before the
`BNodeStore` so that it outlives it.

Node lifecycle and age list:

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/BNodeStore/CapacityForBlock` | Called at run time, `capacityForBlock` returns `InternalCapacity` and `LeafCapacity`; each is the largest capacity whose layout fits in `BlockSize`, since one more entry does not fit. A layout of one byte per entry is capped at 64, and a layout that does not fit even one entry gives 0. | PASS |
| `/resolver/BNodeStore/AllocateLeafInitializesNode` | A new leaf has kind `Leaf`, generation 1, the given `maxCV`, no entries, no parent, no age links, all `LeafCapacity` slots free, a zeroed order and no left or right neighbour. | PASS |
| `/resolver/BNodeStore/AllocateInternalInitializesNode` | A new internal node has kind `Internal`, generation 1, the given `maxCV`, no entries, no parent, no age links, all `InternalCapacity` slots free and a zeroed order. | PASS |
| `/resolver/BNodeStore/GenerationsIncreaseAcrossKinds` | Leaves and internal nodes share one generation sequence: 1, 2, 3. | PASS |
| `/resolver/BNodeStore/AgeListFollowsAllocationOrder` | Three nodes of both kinds are linked in allocation order through `agePrevious` and `ageNext`, with no link before the first or after the last. | PASS |
| `/resolver/BNodeStore/AllocationFailsWhenPoolExhausted` | With every block of a one-page pool used, allocating a leaf or an internal node returns `nullptr`, consumes no generation and leaves the age list unchanged; after a release, the next node gets the next generation and follows the newest remaining node. | PASS |
| `/resolver/BNodeStore/ReleaseReturnsBlockToPool` | Releasing a leaf returns its block to the pool; the same address is reused for an internal node, initialized as such and with a generation higher than the leaf's, read before the release; releasing it restores the free-block count. | PASS |
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

Entries within a node. Every insertion and erasure test checks the node
after each operation against a reference sequence kept in a
`std::vector`: `count`, the range and commit version (leaves) or the
separator, separator commit version and child (internal nodes) at each
rank through `order`, that no slot appears twice in `order`,
that `freeSlots` holds exactly the unused slots, and `hasSpace`. In
internal nodes it also checks that every child still in the node points
to it as `parent` and holds its slot as `parentSlot`, and that
`maxSubtreesCV` of the slot equals the child's `maxCV`. Leaf entries
alternate point ranges and general ranges, so that leaves hold both
kinds of record.

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
| `/resolver/BNodeStore/FindKeyInEmptyNodes` | In an empty leaf and an empty internal node, searching an external key returns false and rank 0. | PASS |
| `/resolver/BNodeStore/FindKeyInLeaf` | Ten ranges, alternately points and general ranges, with begins at the ten search keys, inserted in the order 5, 2, 8, 0, 9, 3, 7, 1, 6, 4 of their sorted position, so that `order` is not the identity; the search compares with each range's begin. Each key, searched as an external `StringRef` over a local array and not stored, is found with its rank. Absent keys before the first, between each pair and after the last return false and the rank where they would be inserted, as do a proper prefix of every key, a key longer than an existing one, and the empty key. | PASS |
| `/resolver/BNodeStore/FindKeyInInternalNode` | The same in an internal node, whose separators are single keys. | PASS |

The counts of full and empty nodes reached by the interleaved tests come
from replaying their random decisions with the same seeds, not from the
test output.

41 BNodeStore tests passed, 0 failed. The full run, including the 13
PagePool tests and the 35 KeyStore tests, reports 89 passed and 0
failed.

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
  lower than the `maxCV` of the newest node.
- `insertAt` receives a non-null node with a free slot and a rank no
  greater than its entry count; for an internal node, also a non-null
  child.
- `eraseAt` receives a non-null node and a rank lower than its entry
  count.
- `findKey` on a leaf receives a non-null node.

These preconditions are part of the interface but are not checked:

- `findKey` requires the entries of the node to be in key order through
  `order`; `insertAt` stores an entry at the rank it is given and does
  not compare keys. The tests insert keys at their sorted rank.
- `eraseAt` on an internal node leaves the removed child unchanged,
  including its `parent` and `parentSlot`. Relinking or releasing that
  child is the caller's responsibility, because splits and erasures
  need different handling.

## Coverage

Coverage was collected running only the BNodeStore tests.

Most of the store is defined in `bnodestore.h` as `inline` functions;
`bnodestore.cpp` holds only the constructor and the destructor. With
`--coverage`, each translation unit keeps its own counters for an
inline function and the linker keeps one copy, so gcov is used only for
`bnodestore.cpp`. llvm-cov merges the counters of all copies and is
used for `bnodestore.h`.

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
| `bnodestore.h` | 97.56% of 82 | 100.00% of 17 | 100.00% of 178 | 100.00% of 28 |

These are gcov and llvm-cov metrics. They are not a measurement of
MC/DC, and full coverage does not show that the code is free of
errors. "Taken at least once" does not mean that every decision was
taken in both directions. Apple clang 15 provides no condition
coverage equivalent to GCC's `-fcondition-coverage`.

gcov counts branches between the basic blocks of the generated code,
including the failure paths of the `assert` checks and the exception
edges the compiler adds. llvm-cov counts the true and false outcomes of
each condition written in the source outside the `assert` checks, whose
conditions come from a macro in a system header: its 30 branches are
the 15 conditions of the store, each taken in both directions.

The 17 functions in `bnodestore.h` are `hasSpace` in each node layout,
the template `capacityForBlock`, and the 14 inline member functions of
`BNodeStore`. llvm-cov reports 2 of its 82 regions as not covered, both
in `capacityForBlock`. `InternalCapacity` and `LeafCapacity` evaluate it
at compile time, so `CapacityForBlock` calls it at run time to execute
it, including on two layouts defined in the test that reach the cap of
64 and the result 0. The two uncovered regions start at the opening
brace of an `if constexpr` branch (lines 81 and 83). In each
instantiation the compiler keeps only one of the three branches and
discards the others, so the discarded regions have no code to execute.
The tests check the value returned through each branch: 0 through
`C - 1`, 64 through the cap, and 40 and 59 through the recursion.

### Decisions

The decisions of the store outside the `assert` checks have a single
condition. llvm-cov reports both outcomes covered for each:

| Location | Condition | True | False |
|---|---|---|---|
| `bnodestore.cpp:11` | Destructor: nodes remain | 622 | 40 |
| `bnodestore.h:177` | No block for a leaf | 1 | 611 |
| `bnodestore.h:189` | No block for an internal node | 1 | 22 |
| `bnodestore.h:201` | Empty age list on insertion | 42 | 596 |
| `bnodestore.h:210` | Unlinked node is the newest | 7 | 9 |
| `bnodestore.h:215` | Unlinked node is the oldest | 12 | 4 |
| `bnodestore.h:231` | `updateCV` with the current value | 1 | 6 |
| `bnodestore.h:236` | Updated node is not the newest | 5 | 1 |
| `bnodestore.h:243` | `oldestCV` on an empty store | 2 | 3 |
| `bnodestore.h:302` | `findKey`, internal node: range not empty | 80 | 15 |
| `bnodestore.h:304` | `findKey`, internal node: key found | 10 | 70 |
| `bnodestore.h:308` | `findKey`, internal node: key before the middle entry | 45 | 25 |
| `bnodestore.h:328` | `findKey`, leaf: range not empty | 80 | 15 |
| `bnodestore.h:330` | `findKey`, leaf: key found | 10 | 70 |
| `bnodestore.h:334` | `findKey`, leaf: key before the middle entry | 45 | 25 |

The `unlinkNode` conditions on lines 210 and 215 are covered by
releasing the only, oldest, middle and newest node, and by moving the
oldest and a middle node in `updateCV`. `insertAt` and `eraseAt` have no
conditions outside their `assert` checks; llvm-cov reports every one of
their lines executed.

### Uncovered generated branches

gcov, which covers only `bnodestore.cpp`, reports 2 untaken branches.
Their origin was checked in the LLVM IR that Apple clang 15 generates
for `bnodestore.cpp` at `-O0` with `--coverage`.

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

- The store has no accessor for the newest node. The age list is
  observed through the `agePrevious` and `ageNext` links of each node,
  and the newest node through the predecessor of the next allocated
  node.
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
  for the full suite (89 tests, PagePool, KeyStore and BNodeStore). It
  is the same run as `pagepool-tests.txt` and `keystore-tests.txt`.
- [Coverage summary](evidence/bnodestore-coverage.txt): gcov output
  after running only `./tests.exe /resolver/BNodeStore`. The filter
  does not appear in the gcov output; it is recorded in
  [Reproduction](#reproduction).
- [Annotated coverage](evidence/bnodestore.cpp.gcov): from the same gcov
  run.
- [llvm-cov summary](evidence/bnodestore-llvm-report.txt): llvm-cov
  report after running only
  `build/coverage-llvm/tests.exe /resolver/BNodeStore`. The filter does
  not appear in the output.
- [llvm-cov annotated coverage](evidence/bnodestore-llvm-show.txt):
  `bnodestore.cpp` and `bnodestore.h` with line and branch counts, from
  the same llvm-cov run.
