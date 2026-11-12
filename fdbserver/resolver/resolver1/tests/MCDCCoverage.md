# MC/DC validation

## Scope

This report measures the modified condition/decision coverage (MC/DC)
reached by resolver1's test suite on the production code of the final
Resolver v3: `pagepool.cpp`, `pagepool.h`, `keystore.cpp`,
`bnodestore.cpp`, `bnodestore.h`, `tempconflictbtree.cpp` and
`tempconflictbtree.h`. The build is the published configuration: write
tasks in 2 groups of 8 (`RESOLVER1_WRITE_LG` 8, `RESOLVER1_WRITE_GROUPS`
2) and 2 KiB blocks. The measurement runs all 240 registered tests once.
Every instrumented condition is listed with its independence pair, or,
when it has none, with its class and the reason.

The report also gives a separate measurement of the build with
`RESOLVER1_WRITE_LG` 0, which is not added to the figures of the
published configuration, and the test matrix run on the measured tree.

The behaviour of each component, and its gcov and llvm-cov line and
branch coverage, are validated in the component catalogs:
[PagePoolTests.md](PagePoolTests.md), [KeyStoreTests.md](KeyStoreTests.md),
[BNodeStoreTests.md](BNodeStoreTests.md) and
[TempConflictBTreeTests.md](TempConflictBTreeTests.md). The integration
of resolver1 in FoundationDB is outside the scope of this report.

## Source and environment

- Source commit: `02d354f928143a45bb51cada4f06da11da628589` ("Add MC/DC
  coverage tests for final Resolver v3"), tree
  `c23e51ad3c71a012360f483824afc107dd1de0c7`, parent `1233986b`. Its
  production files are identical to those of `f866c98d`; it adds eight
  tests to `tests/pagepooltests.cpp` and
  `tests/tempconflictbtreetests.cpp`
  ([diffs](evidence/mcdc/diffs/), [file hashes](evidence/mcdc/SOURCES-SHA256.txt)).
- Working tree: clean. Every measurement ran on a `git archive` of that
  commit.
- Platform: macOS 27.0.1 (26A434), arm64 (Apple M3 Pro)
- Compiler: clang 20.1.8 from the LLVM release archive
  `LLVM-20.1.8-macOS-ARM64.tar.xz` (llvm-project
  `87f0227cb60147a26a1eeb4fb06e3b505e9c7261`), with its `lld`, and the
  macOS 14.4 SDK ([toolchain](evidence/mcdc/toolchain.txt))
- Language standard: C++20
- Build flags: `-fuse-ld=lld -isysroot $(xcrun --show-sdk-path) -I. -std=c++20 -Wall -Wextra -Wpedantic -O0`
- Coverage tool and version: `llvm-cov` and `llvm-profdata` 20.1.8, from
  the same archive
- Coverage build flags: `-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc -fcoverage-compilation-dir=.`,
  added to the build flags; the LG 0 build also defines
  `-DRESOLVER1_WRITE_LG=0`
- Test matrices: Apple clang 15.0.0 on the same Mac; clang 19.1.5 with
  libc++ in the `foundationdb/build:rockylinux9-latest` image on Linux
  (AMD Ryzen 7 7700). See [Validation matrix](#validation-matrix).

Apple clang 15, used by the component catalogs, has no MC/DC
instrumentation; no MC/DC figure in this report comes from it.

## Reproduction

Run from the project root (`fdbserver/resolver/resolver1`):

```bash
make test
```

MC/DC collection, with `L` the `bin` directory of LLVM 20.1.8:

```bash
X=tests/evidence/mcdc/scripts
bash $X/mcdc.sh $L tests/evidence/mcdc/2x8
bash $X/attribute_vectors.sh $L . tests/evidence/mcdc/2x8/tests.txt tests/evidence/mcdc/2x8 $X
python3 -I $X/build_catalog.py tests/evidence/mcdc/2x8/mcdc-conditions.tsv \
	tests/evidence/mcdc/2x8/vectors-by-test.tsv tests/evidence/mcdc/2x8/mcdc-catalog
python3 -I $X/provisional_count.py tests/evidence/mcdc/2x8/llvm-cov-show.txt
bash $X/mcdc.sh $L tests/evidence/mcdc/lg0 -DRESOLVER1_WRITE_LG=0
```

`mcdc.sh` compiles and links the suite in one command, runs every test
once, and writes the `llvm-cov` report, `show` and `export` of the
production files ([commands](evidence/mcdc/2x8/build-flags.txt)). Its
last step, `mcdc_extract.py`, reads the MC/DC records of `llvm-cov show`
and checks its per-file totals against the `llvm-cov export` summary; it
fails if they differ. `attribute_vectors.sh` runs each test alone, through
an evidence harness (`exactmain.cpp`) that runs only the test whose name
equals its argument, because the runner of the suite selects by prefix
and 17 test names are a prefix of another. `build_catalog.py` joins the
records, the vectors of each test and the classification in
`classification.py`, and fails unless the classified conditions are
exactly those without a pair.

The matrices are run by `linux-matrix.sh` and `mac-matrix.sh` in the
same directory.

## Test cases

The measurement runs the 240 registered tests: 19 PagePool, 41
KeyStore, 48 BNodeStore and 132 TempConflictBTree. All passed
([output](evidence/mcdc/2x8/tests.txt)). Each test and the property it
checks are listed in the component catalogs. The eight tests below were
written for this measurement; they are in commit `02d354f9`.

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/PagePool/PageModeLegacyByDefaultAndByName` | With `RESOLVER1_PAGE_MODE` unset and set to `legacy`, a pool of two pages is `Legacy`, with every block available. | PASS |
| `/resolver/PagePool/PageModeUnknownThrows` | An unknown mode throws `std::invalid_argument`. | PASS |
| `/resolver/PagePool/PageModeExplicitWithoutPages` | `base`, `thp` and `hugetlb` with less than a page select that backing and leave the pool empty. | PASS |
| `/resolver/PagePool/PageModeBaseWithPages` | `base` with two pages maps an aligned pool on Linux and throws `std::runtime_error` elsewhere. | PASS |
| `/resolver/TempConflictBTree/AddWritesUnderPressureAgainstModel` | 200 batches of up to 30 writes, with keys of 3 to 302 bytes, in a pool of four pages, for G = 16 and 32: after every batch the live entries equal a model, and reads above the cut conflict as the model says. | PASS |
| `/resolver/TempConflictBTree/CheckReadsWithNoOperationOnNonemptyTree` | A batch whose reads are all empty, or all too old, on a tree with history: statuses as the reference, tree unchanged. | PASS |
| `/resolver/TempConflictBTree/AddWritesIntoExpiredRootLeaf` | A write into a root leaf whose entries all expired is stored with its CV. | PASS |
| `/resolver/TempConflictBTree/AddWritesInAPoolSmallerThanTheReserve` | Batches of writes into a one-page pool, smaller than the write reserve of 35 blocks with 2 KiB blocks or larger: the entries above the cut equal the model after every batch. | PASS |

## Coverage

MC/DC was collected for the production files listed in
[Scope](#scope), running the full suite once, without a filter.

| File | Conditions | With an independence pair | Without | MC/DC |
|---|---|---|---|---|
| `bnodestore.h` | 5 | 1 | 4 | 20.00% |
| `keystore.cpp` | 27 | 25 | 2 | 92.59% |
| `pagepool.cpp` | 7 | 4 | 3 | 57.14% |
| `tempconflictbtree.cpp` | 171 | 123 | 48 | 71.93% |
| Total | 210 | 153 | 57 | 72.86% |

`bnodestore.cpp`, `pagepool.h` and `tempconflictbtree.h` have no decision
with two or more conditions. The figures are those of `llvm-cov report
-show-mcdc-summary` ([report](evidence/mcdc/2x8/llvm-cov-report.txt)), and
`mcdc_extract.py` obtains the same per-file counts from the records of
`llvm-cov show` ([summary](evidence/mcdc/2x8/mcdc-summary.txt)).

These are LLVM source-based MC/DC metrics, as `llvm-cov` 20 defines them:

- A decision is a boolean expression of two or more conditions; a single
  condition is not instrumented for MC/DC. Its branch coverage is in the
  component catalogs.
- A condition has an independence pair when two executed test vectors
  differ in that condition, and in no other condition evaluated in both,
  and give different decisions. Conditions not evaluated because of
  short-circuit evaluation are shown as `-`.
- Each template instantiation is a separate record. Counting each
  condition of a source decision once, it has a pair when any
  instantiation shows one. `mcdc_extract.py` merges the records with that
  rule and obtains the per-file counts of `llvm-cov export`.
- A condition that is a constant in every instantiation is reported as
  constant folded and is not counted. There is one,
  `bnodestore.h:74` C2, `C % 64 == 0`.
- The conditions of an `assert` are instrumented and counted. llvm-cov
  places them at the position of the macro call.

The class of every counted condition; each of the 57 without a pair has
exactly one class, defined in
[Independent condition checks](#independent-condition-checks):

| Class | `bnodestore.h` | `keystore.cpp` | `pagepool.cpp` | `tempconflictbtree.cpp` | Total |
|---|---|---|---|---|---|
| covered: independence pair | 1 | 25 | 4 | 123 | 153 |
| assert condition | 4 | 2 | 3 | 18 | 27 |
| unreachable (invariant) | 0 | 0 | 0 | 16 | 16 |
| defensive | 0 | 0 | 0 | 2 | 2 |
| LG=0 only | 0 | 0 | 0 | 8 | 8 |
| reachability not established | 0 | 0 | 0 | 4 | 4 |
| Total | 5 | 27 | 7 | 171 | 210 |

The `assert` conditions stay in the denominator. Their false outcome
aborts the process, so a test cannot show their independence; they are
neither counted as covered nor removed from the figures.

The figures 197 and 56 that appeared in a preliminary count are not
measurements of this report. That count keyed each condition by its file,
the first line of its decision and its own line and column. The 13
`assert` decisions place all their conditions (two each, three on
`pagepool.cpp:20`) at the position of the macro call, so 14 conditions
merged into others, and the constant-folded condition was included:
210 − 14 + 1 = 197 keys. It was applied to a run of the suite before the
eight tests above, in which 56 of the keys had no pair. On the
measurement of this report the same keying gives 197 keys and 44 without
a pair ([provisional count](evidence/mcdc/2x8/provisional-count.txt),
`provisional_count.py`).

### Independent condition checks

The catalog [mcdc-catalog.tsv](evidence/mcdc/2x8/mcdc-catalog.tsv) lists
every instrumented condition (210 counted and the constant-folded one):
file, line, decision, condition and position, class, the independence
pair as two test vectors, the first test in registration order whose run
alone executes each vector of the pair, all vectors executed by the
suite, and, for a condition without a pair, the justification. A vector
is written `{C1 C2 ... = decision}` with `T`, `F`, `-` (not evaluated)
and `C` (constant). The per-test vectors are in
[vectors-by-test.tsv](evidence/mcdc/2x8/vectors-by-test.tsv): 240 runs,
one per test, in which 238 tests execute at least one instrumented
decision (`/resolver/BNodeStore/CapacityForBlock` and
`/resolver/BNodeStore/FreeSlotsSize` execute none). Their union equals,
decision by decision, the vectors of the full run.

The pairs assume the values of the measured build: `BlockSize` 2048,
`sizeof(KeyBlockHeader)` 24, `LeafCapacity` 59, `InternalCapacity` 40,
32 blocks per 64 KiB page, `RESOLVER1_WRITE_LG` 8,
`RESOLVER1_WRITE_GROUPS` 2, and `checkReads` instantiated for G = 16, 24
and 32, of which the tests run 16 and 32. `RESOLVER1_PAGE_MODE` is unset
except inside the four `PageMode` tests, which restore it.

The classes:

- **covered: independence pair**: `llvm-cov` reports a pair.
- **assert condition**: a condition of an `assert`. Every execution is
  true; the false outcome aborts.
- **unreachable (invariant)**: the outcome needed by every pair cannot
  occur while the interface preconditions and the invariants cited hold.
  Each justification names the lines that establish it; not having been
  reached is not a reason for this class.
- **defensive**: a check whose true outcome reports the violation of an
  invariant (here, a `std::logic_error`). No fault was injected.
- **LG=0 only**: the condition is in code that the published build
  compiles but calls only when `RESOLVER1_WRITE_LG` is 0. Its outcomes
  in that build are given by the separate measurement below.
- **reachability not established**: no run produced the outcome needed,
  and no invariant excluding it was established.

Pairs in which a vector is executed first by one of the eight tests
added in `02d354f9`:

| Location | Condition | Pair | First test for each vector |
|---|---|---|---|
| `pagepool.cpp:28:6` | C1/2 | `{F F = F} / {T - = T}` | `{F F = F}`: `PageModeUnknownThrows`; `{T - = T}`: `AllocateLeafInitializesNode` |
| `pagepool.cpp:28:25` | C2/2 | `{F F = F} / {F T = T}` | `{F F = F}`: `PageModeUnknownThrows`; `{F T = T}`: `PageModeLegacyByDefaultAndByName` |
| `pagepool.cpp:39:6` | C1/2 | `{F F = F} / {T - = T}` | `{F F = F}`: `PageModeBaseWithPages`; `{T - = T}`: `AllocateLeafInitializesNode` |
| `pagepool.cpp:39:37` | C2/2 | `{F F = F} / {F T = T}` | `{F F = F}`: `PageModeBaseWithPages`; `{F T = T}`: `PageModeExplicitWithoutPages` |
| `tempconflictbtree.cpp:145:35` | C2/3 | `{T F - = F} / {T T T = T}` | `{T F - = F}`: `AddWritesInAPoolSmallerThanTheReserve`; `{T T T = T}`: `AddWriteKinds` |
| `tempconflictbtree.cpp:153:4` | C1/2 | `{F - = F} / {T T = T}` | `{F - = F}`: `AddWriteAgainstModel`; `{T T = T}`: `AddWritesUnderPressureAgainstModel` |
| `tempconflictbtree.cpp:153:35` | C2/2 | `{T F = F} / {T T = T}` | `{T F = F}`: `AddWriteKinds`; `{T T = T}`: `AddWritesUnderPressureAgainstModel` |
| `tempconflictbtree.cpp:304:9` | C1/4 | `{F - - - = F} / {T F F T = T}` | `{F - - - = F}`: `AddWriteKinds`; `{T F F T = T}`: `AddWritesUnderPressureAgainstModel` |
| `tempconflictbtree.cpp:305:48` | C4/4 | `{T F F F = F} / {T F F T = T}` | `{T F F F = F}`: `AddWritesAgainstReference`; `{T F F T = T}`: `AddWritesUnderPressureAgainstModel` |
| `tempconflictbtree.cpp:776:3` | C2/2 | `{T F = F} / {T T = T}` | `{T F = F}`: `AddWritesInAPoolSmallerThanTheReserve`; `{T T = T}`: `AddWritesThrowsWhenThePoolCannotHoldAWrite` |
| `tempconflictbtree.cpp:2160:7` | C1/3 | `{F F F = F} / {T - - = T}` | `{F F F = F}`: `AddWriteKinds`; `{T - - = T}`: `AddWritesUnderPressureAgainstModel` |
| `tempconflictbtree.cpp:2160:53` | C3/3 | `{F F F = F} / {F F T = T}` | `{F F F = F}`: `AddWriteKinds`; `{F F T = T}`: `AddWritesIntoExpiredRootLeaf` |
| `tempconflictbtree.cpp:2515:26` | C2/2 | `{F F = F} / {F T = T}` | `{F F = F}`: `AddWriteKinds`; `{F T = T}`: `CheckReadsWithNoOperationOnNonemptyTree` |

Conditions without an independence pair, and the constant-folded one.
Line numbers are those of the measured tree; unless another file is
named, they are lines of `tempconflictbtree.cpp`. `ConflictSet.cpp` is
`fdbserver/resolver/ConflictSet.cpp` of the same commit, whose code calls
resolver1 inside FoundationDB.

| Location | Decision, condition | Condition | Observed vectors | Class | Justification |
|---|---|---|---|---|---|
| `bnodestore.h:74` | `74:10-74:38` C2 of 2 | `C % 64 == 0` | {F C = F}, {F C = T}, {T C = T} | constant folded (not counted) | `C` is the template parameter of `FreeSlots<C>`, so the condition is a compile-time constant in each instantiation. llvm-cov reports it as constant folded and leaves it out of the 210 conditions. |
| `bnodestore.h:356` | `356:2-356:2` C1 of 2 | `newestNode_ == nullptr` | {F T = T}, {T - = T} | assert condition | Condition of `assert(newestNode_ == nullptr \|\| cv >= newestNode_->maxCV)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `bnodestore.h:356` | `356:2-356:2` C2 of 2 | `cv >= newestNode_->maxCV` | {F T = T}, {T - = T} | assert condition | Condition of `assert(newestNode_ == nullptr \|\| cv >= newestNode_->maxCV)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `bnodestore.h:372` | `372:2-372:2` C1 of 2 | `newestNode_ == nullptr` | {F T = T}, {T - = T} | assert condition | Condition of `assert(newestNode_ == nullptr \|\| cv >= newestNode_->maxCV)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `bnodestore.h:372` | `372:2-372:2` C2 of 2 | `cv >= newestNode_->maxCV` | {F T = T}, {T - = T} | assert condition | Condition of `assert(newestNode_ == nullptr \|\| cv >= newestNode_->maxCV)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `keystore.cpp:247` | `247:2-247:2` C1 of 2 | `isKeyValid(kr1)` | {T T = T} | assert condition | Condition of `assert(isKeyValid(kr1) && isKeyValid(kr2))`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `keystore.cpp:247` | `247:2-247:2` C2 of 2 | `isKeyValid(kr2)` | {T T = T} | assert condition | Condition of `assert(isKeyValid(kr1) && isKeyValid(kr2))`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `pagepool.cpp:20` | `20:2-20:2` C1 of 3 | `blockSize >= sizeof(FreeBlock)` | {T T T = T} | assert condition | Condition of `assert(blockSize >= sizeof(FreeBlock) && blockSize <= PageSize && blockSize % alignof(FreeBlock) == 0)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `pagepool.cpp:20` | `20:2-20:2` C2 of 3 | `blockSize <= PageSize` | {T T T = T} | assert condition | Condition of `assert(blockSize >= sizeof(FreeBlock) && blockSize <= PageSize && blockSize % alignof(FreeBlock) == 0)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `pagepool.cpp:20` | `20:2-20:2` C3 of 3 | `blockSize % alignof(FreeBlock) == 0` | {T T T = T} | assert condition | Condition of `assert(blockSize >= sizeof(FreeBlock) && blockSize <= PageSize && blockSize % alignof(FreeBlock) == 0)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:59` | `59:8-59:69` C1 of 2 | `!info.tooOld` | {T F = F}, {T T = T} | unreachable (invariant) | Always true. `readWriteBegins` is built only by `checkReads` (cleared at 1814, filled at 1861). A begin of a transaction already too old is skipped (1838); otherwise the transaction is marked too old and skipped (1839-1845). The test at 1839 depends only on the transaction and on `reclaimCV_`, which `checkReads` does not change (tempconflictbtree.h:90), so all begins of a transaction take the same branch and none of a too-old transaction is pushed. resolver1 sets `tooOld` only at 1841; the caller sets it before the batch (ConflictSet.cpp:827-842) and, between `checkReads` and `addWrites`, changes only `transactionConflictStatus` (ConflictSet.cpp:942-963, 988-1000). Precondition: `addWrites` applies the batch that the preceding `checkReads` prepared (tempconflictbtree.h:88-109). |
| `tempconflictbtree.cpp:134` | `134:10-134:65` C1 of 2 | `t.write.first.size() != 0` | {T - = T} | unreachable (invariant) | Both pairs need `{F F = F}`, an accepted interval whose begin and end keys are both empty. An interval is accepted only with begin index < end index (65), and merging only raises the end (66-68). `sortPoints` puts every end before every begin at equal keys (ConflictSet.cpp:98-100, 126-142), so begin index < end index implies begin key < end key, and the end key is not empty: whenever C1 is false, C2 is true. |
| `tempconflictbtree.cpp:134` | `134:10-134:65` C2 of 2 | `t.write.second.size() != 0` | {T - = T} | unreachable (invariant) | As C1 of this decision: the end key of an accepted interval is never empty, so the outcome is never false. |
| `tempconflictbtree.cpp:143` | `143:7-143:48` C1 of 2 | `!t.hasSavedLeaf` | {F F = F}, {F T = T} | unreachable (invariant) | Always false. `hasSavedLeaf` is false only in a default `WriteTask` (102), which exists only between 170 and `resetSearch` at 174, which sets it (159, 140). `savedLeafIsCurrent` is called only inside `advance` (247, 287; assertions 388, 396, 429), after `fill`. |
| `tempconflictbtree.cpp:145` | `145:10-146:50` C1 of 3 | `(generation & 1) != 0` | {T F - = F}, {T T F = F}, {T T T = T} | reachability not established | False only if the block of the saved leaf was released (even generation) and not reused when the task checks it. A leaf can be released while another task has saved it (reclamation in a head task's reservation, or removal of an emptied leaf); no run produced an even generation here, and no invariant excluding it was established. C2 of this decision has a pair: a reused block (another generation) was observed. |
| `tempconflictbtree.cpp:231` | `231:10-231:61` C2 of 2 | `hintLeaf(t.scan->left)` | {F - = F}, {T T = T} | unreachable (invariant) | Evaluated only when the scan leaf has one entry and no right neighbour (229-231), and false only when its left link is null. The scan starts in the cursor leaf at rank + 1 after the insertion (442-443), so that leaf holds the inserted entry and the scanned one (two or more entries). A later scan leaf is reached through `right` (455), and the cursor leaf, which keeps the inserted entry, stays linked on its left. Always true. |
| `tempconflictbtree.cpp:236` | `236:3-236:3` C1 of 2 | `!t.owned` | {F T = T}, {T - = T} | assert condition | Condition of `assert(!t.owned \|\| head)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:236` | `236:3-236:3` C2 of 2 | `head` | {F T = T}, {T - = T} | assert condition | Condition of `assert(!t.owned \|\| head)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:275` | `275:5-275:5` C1 of 2 | `head` | {T T = T} | assert condition | Condition of `assert(head && t.owned)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:275` | `275:5-275:5` C2 of 2 | `t.owned` | {T T = T} | assert condition | Condition of `assert(head && t.owned)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:286` | `286:9-287:28` C3 of 3 | `!savedLeafIsCurrent(t)` | {F F F = F}, {F T - = T}, {T - - = T} | reachability not established | Needs the reservation at 283 to change the saved leaf (generation or mutation epoch) while the cursor leaf (C1) and the cut (C2) stay the same. No run produced it, and it was not shown that a reservation changes leaves only by raising the cut. |
| `tempconflictbtree.cpp:304` | `304:9-305:76` C2 of 4 | `cleanLeafCV_ != reclaimCV_` | {F - - - = F}, {T F F F = F}, {T F F T = T}, {T F T - = T} | reachability not established | Needs a task that is not the head to inspect a leaf after the cut was raised and before any `cleanExpired` call resynchronized `cleanLeafCV_` (1367-1369). No run produced it, and no invariant excluding it was established. |
| `tempconflictbtree.cpp:329` | `329:12-330:57` C1 of 2 | `t.scanRank < t.scan->header.count` | {T F = F}, {T T = T} | reachability not established | False only when every entry of the right neighbour inspected at `InspectRight` is expired; that neighbour is not compacted before this loop. No run produced it, and no invariant excluding it was established. |
| `tempconflictbtree.cpp:404` | `404:10-404:95` C1 of 2 | `t.scan->cv[slot] > reclaimCV_` | {T T = T} | unreachable (invariant) | Always true. With rank > 0 the scan is the cursor leaf, compacted under the current cut: a head task cleans it at `Inspect` (308) or after moving right (348); a task that is not the head searches only leaves already clean under the current cut (304-306) and parks at `SearchReady` (386); a change of the cut resets the search (245-252, and 286-292 after the task's own reservation). `cleanExpired` removes every entry with CV <= `reclaimCV_` (1365-1391), and an entry inserted later carries the batch's CV, above the cut. |
| `tempconflictbtree.cpp:404` | `404:10-404:95` C2 of 2 | `hintKey(t, t.scan->ranges[slot], true, Step::Insert)` | {T T = T} | unreachable (invariant) | `hintKey` returns false only for length 0 (196). With `wholeRange` the length is at least `endStart + endLength`, the length of the stored end key. Every stored range has begin < end: a write is inserted (430) only after 276 skips begin >= end, and an exterior keeps the part of an existing range on one side of a key of the write. So the end key is not empty and the condition is always true. |
| `tempconflictbtree.cpp:422` | `422:10-422:95` C2 of 2 | `hintKey(t, t.scan->ranges[slot], true, Step::Insert)` | {F - = F}, {T T = T} | unreachable (invariant) | As 404 C2: the stored end key is never empty, so `hintKey` returns true whenever it is evaluated. |
| `tempconflictbtree.cpp:518` | `518:7-518:7` C1 of 2 | `which == 0` | {T T = T} | assert condition | Condition of `assert(which == 0 && i == group.first)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:518` | `518:7-518:7` C2 of 2 | `i == group.first` | {T T = T} | assert condition | Condition of `assert(which == 0 && i == group.first)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:617` | `617:8-617:67` C1 of 2 | `info.tooOld` | none | LG=0 only | In `addWritesSequential`, called only when `RESOLVER1_WRITE_LG` is 0 (`if constexpr`, 29-31); no vector in the 2x8 build. In the separate LG=0 measurement it has no pair either, for the reason given at 59 C1: no entry of `readWriteBegins` belongs to a too-old transaction. |
| `tempconflictbtree.cpp:617` | `617:8-617:67` C2 of 2 | `transactionConflictStatus[entry.transaction]` | none | LG=0 only | In `addWritesSequential`, called only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement. |
| `tempconflictbtree.cpp:627` | `627:8-627:45` C1 of 2 | `hasPending` | none | LG=0 only | In `addWritesSequential`, called only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement. |
| `tempconflictbtree.cpp:627` | `627:8-627:45` C2 of 2 | `beginIndex > pendingEnd` | none | LG=0 only | In `addWritesSequential`, called only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement. |
| `tempconflictbtree.cpp:661` | `661:2-661:2` C1 of 2 | `writeDestinationsActive_` | {T T = T} | assert condition | Condition of `assert(writeDestinationsActive_ && group < writeLeaves.size())`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:661` | `661:2-661:2` C2 of 2 | `group < writeLeaves.size()` | {T T = T} | assert condition | Condition of `assert(writeDestinationsActive_ && group < writeLeaves.size())`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:712` | `712:6-712:64` C2 of 2 | `activeWriteCursor_ == nullptr` | {F - = F}, {T T = T} | unreachable (invariant) | Its pair needs `{T F = F}`: destinations inactive while a cursor is active. `activeWriteCursor_` is set only at 267 (inside `addWrites`) and 816 (inside `addWritesSequential`), and reset by guards (258, 819) before those functions return. Both activate the destinations first (39, 550; `writeLeaves` is not empty by 35-37 and 544-548, so 646 sets the flag) and clear them only when they return (42, 553); the only other clear is at the start of `checkReads` (1812). Unreachable in every build. |
| `tempconflictbtree.cpp:745` | `745:2-745:2` C1 of 2 | `ticket >= writeLeafBufferFirst_` | {T T = T} | assert condition | Condition of `assert(ticket >= writeLeafBufferFirst_ && ticket < writeLeafBufferNext_)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:745` | `745:2-745:2` C2 of 2 | `ticket < writeLeafBufferNext_` | {T T = T} | assert condition | Condition of `assert(ticket >= writeLeafBufferFirst_ && ticket < writeLeafBufferNext_)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:753` | `753:3-753:3` C1 of 2 | `writeLeaves.empty()` | {F T = T}, {T - = T} | assert condition | Condition of `assert(writeLeaves.empty() \|\| writeLeaves.back().firstWrite < buffered.group.firstWrite)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:753` | `753:3-753:3` C2 of 2 | `writeLeaves.back().firstWrite < buffered.group.firstWrite` | {F T = T}, {T - = T} | assert condition | Condition of `assert(writeLeaves.empty() \|\| writeLeaves.back().firstWrite < buffered.group.firstWrite)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:803` | `803:6-803:41` C1 of 2 | `leaf == nullptr` | {F - = F}, {T F = F} | defensive | Both pairs need `{T T = T}`, which throws `std::logic_error("missing prepared write destination")`: a cursor without a leaf in a non-empty tree. Before `prepareWriteSplit` (389), a null cursor leaf is replaced by `rightmostLeaf_` (284; `resetSearch` anchors on it at 151), and `rightmostLeaf_` is null only when `root_` is null (1187-1189, 1424-1427). The check guards that invariant; no fault was injected to violate it. |
| `tempconflictbtree.cpp:803` | `803:6-803:41` C2 of 2 | `root_ != nullptr` | {F - = F}, {T F = F} | defensive | As C1 of this decision: the true outcome is the defensive `std::logic_error` for a cursor without a leaf in a non-empty tree. |
| `tempconflictbtree.cpp:838` | `838:12-839:53` C1 of 2 | `first < right->header.count` | none | LG=0 only | In `addWrite`, called only from `addWritesSequential` (576), only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. In the separate LG=0 measurement it has no pair: false only when every entry of a right neighbour is expired, as at 329 C1. |
| `tempconflictbtree.cpp:838` | `838:12-839:53` C2 of 2 | `right->cv[right->order[first]] <= reclaimCV_` | none | LG=0 only | In `addWrite`, called only when `RESOLVER1_WRITE_LG` is 0 (29-31, 576); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement. |
| `tempconflictbtree.cpp:926` | `926:7-927:54` C1 of 2 | `leaf->cv[slot] > reclaimCV_` | none | LG=0 only | In `eraseCoveredRanges`, called only from `addWrite` (863), only when `RESOLVER1_WRITE_LG` is 0 (29-31); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement. |
| `tempconflictbtree.cpp:926` | `926:7-927:54` C2 of 2 | `keyStore_.compareEnd(end, leaf->ranges[slot]) < 0` | none | LG=0 only | In `eraseCoveredRanges`, only when `RESOLVER1_WRITE_LG` is 0 (29-31, 863); no vector in the 2x8 build. It has a pair in the separate LG=0 measurement. |
| `tempconflictbtree.cpp:1414` | `1414:6-1414:40` C2 of 2 | `!needsSplit(parent)` | {F - = F}, {T T = T} | unreachable (invariant) | Its pair needs a parent that needed a split before (C1) and still needs one after an empty child is erased. An internal node needs a split when `childrenNeedingSplit == freeSlotCount()` (tempconflictbtree.h:318-320). The erased child is empty (1398), so it does not need a split and is not counted (comment at 1409); erasing it (1410) adds a free slot and leaves the count unchanged. Always true when evaluated. |
| `tempconflictbtree.cpp:1519` | `1519:6-1519:74` C2 of 2 | `activeWriteCursor_->leaf == oldNode` | {F - = F}, {T T = T} | unreachable (invariant) | Its pair needs an active cursor and a different leaf being split. `splitLeaf` is called only by `split` (1461), only from `prepareWriteSplit` (807), with `leaf` bound to the cursor's leaf (783) and asserted equal to the split node (1460); a leaf candidate other than the cursor's is discarded when the cursor moves (349, 788-799). `prepareWriteSplit` runs at 389 with the head task's cursor, the active one (267), and at 851 with `addWrite`'s cursor, the active one (816). Unreachable in every build. |
| `tempconflictbtree.cpp:1611` | `1611:2-1611:2` C1 of 2 | `splitDelta >= -1` | {T T = T} | assert condition | Condition of `assert(splitDelta >= -1 && splitDelta <= 1)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:1611` | `1611:2-1611:2` C2 of 2 | `splitDelta <= 1` | {T T = T} | assert condition | Condition of `assert(splitDelta >= -1 && splitDelta <= 1)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:1629` | `1629:4-1629:4` C1 of 2 | `splitDelta > 0` | {F T = T}, {T - = T} | assert condition | Condition of `assert(splitDelta > 0 \|\| parent->header.childrenNeedingSplit > 0)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:1629` | `1629:4-1629:4` C2 of 2 | `parent->header.childrenNeedingSplit > 0` | {F T = T}, {T - = T} | assert condition | Condition of `assert(splitDelta > 0 \|\| parent->header.childrenNeedingSplit > 0)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:1904` | `1904:7-1905:31` C1 of 2 | `transactionInfo[entry.transaction]->tooOld` | {F F = F}, {F T = T} | unreachable (invariant) | Always false: no entry of `readWriteBegins` belongs to a too-old transaction. `readWriteBegins` is built only by `checkReads` (cleared at 1814, filled at 1861). A begin of a transaction already too old is skipped (1838); otherwise the transaction is marked too old and skipped (1839-1845). The test at 1839 depends only on the transaction and on `reclaimCV_`, which `checkReads` does not change (tempconflictbtree.h:90), so all begins of a transaction take the same branch and none of a too-old transaction is pushed. |
| `tempconflictbtree.cpp:1961` | `1961:7-1961:47` C1 of 2 | `length == 0` | {F F = F} | unreachable (invariant) | Always false. `prefetchStoredKeySlow` is called only from `prefetchStoredKey` (2009), after it returns for length 0 (1993). |
| `tempconflictbtree.cpp:1961` | `1961:7-1961:47` C2 of 2 | `!keyStore_.isValid(range)` | {F F = F} | unreachable (invariant) | Always false. The ranges come from 2022, a separator hinted only when its CV is above the cut after `refreshSeparatorDeferred` (2051-2058); from 2068, an entry of a leaf compacted by `cleanReadLeaf` (2172); and from 2076, an entry of a left leaf compacted at 2132 whose CV is above the cut (2139). `checkReads` does not change `reclaimCV_` (tempconflictbtree.h:90), and a key block is reclaimed only when its CV is at or below `reclaimCV_` (tempconflictbtree.h:290-291), so these key bytes are valid. |
| `tempconflictbtree.cpp:2160` | `2160:7-2160:76` C2 of 3 | `leaf == nullptr` | {F F F = F}, {F F T = T}, {T - - = T} | unreachable (invariant) | `bufferTaskLeaf` receives `nullptr` only from 2204, reached only for an internal node without children (2199-2200). Only the root leaf may be empty when an operation returns, and an internal node left without children is removed before (tempconflictbtree.h:18-20; 1419-1443). During `checkReads` emptied leaves are only queued (1933), separators are refreshed without removal (1276-1300), and the cleanup runs after the descent (2525). Always false. |
| `tempconflictbtree.cpp:2425` | `2425:4-2425:4` C1 of 2 | `next > writeFrontier` | {T T = T} | assert condition | Condition of `assert(next > writeFrontier && next <= operationCount)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:2425` | `2425:4-2425:4` C2 of 2 | `next <= operationCount` | {T T = T} | assert condition | Condition of `assert(next > writeFrontier && next <= operationCount)`. Its false outcome calls `__assert_rtn` and aborts the process; every execution is true. llvm-cov places all conditions of the macro at the position of the call. Counted in the 210 and not covered. |
| `tempconflictbtree.cpp:2444` | `2444:8-2444:73` C1 of 2 | `tasks[slot].step == Step::Empty` | {F F = F}, {F T = T} | unreachable (invariant) | A task in `order` has step `Empty` only after 2205, which needs the internal node without children of 2160 C2. Lines 2445 and 2465 set `Empty` on slots that are then freed and left out of `order` (2442-2449, 2461-2466, 2471-2480). Always false. |

### Separate measurement with `RESOLVER1_WRITE_LG` 0

The same suite, built with `-DRESOLVER1_WRITE_LG=0` and otherwise the
same flags, passed 240 of 240 tests
([output](evidence/mcdc/lg0/tests.txt)). This measurement is reported on
its own and is not added to the figures above.

| File | Conditions | With an independence pair | Without | MC/DC |
|---|---|---|---|---|
| `bnodestore.h` | 5 | 1 | 4 | 20.00% |
| `keystore.cpp` | 27 | 25 | 2 | 92.59% |
| `pagepool.cpp` | 7 | 4 | 3 | 57.14% |
| `tempconflictbtree.cpp` | 171 | 96 | 75 | 56.14% |
| Total | 210 | 126 | 84 | 60.00% |

In this build `addWrites` returns after `addWritesSequential` (lines
29-31), so the write path of the published build is compiled but never
runs: 49 counted conditions there execute no vector. Of the eight
conditions classed LG=0 only, six have a pair in this build (617 C2,
627 C1 and C2, 838 C2, 926 C1 and C2). 617 C1 and 838 C1 have none, for
the reasons given in the table above; 712 C2 and 1519 C2 have no pair in
this build either.

### Uncovered generated branches

This report measures source conditions, not generated branches. The gcov
and llvm-cov branch items of the same tree, with the distinction between
unreachable branches and possible failures that were not injected, are
classified in the component catalogs.

## Validation matrix

The full suite of the measured tree was built and run in 48
configurations ([summary](evidence/mcdc/matrix/matrix.txt)): write
tasks 2×8 and 2×4, blocks of 1, 2, 4 and 8 KiB, and three builds on each
platform. Every configuration compiled without a warning
(`-Wall -Wextra -Wpedantic`) and passed 240 of 240 tests with no
sanitizer report.

| Platform | Compiler | Builds | Configurations | Result |
|---|---|---|---|---|
| Linux, AMD Ryzen 7 7700 | clang 19.1.5, libc++ (`foundationdb/build:rockylinux9-latest`) | assertions `-g -O1`; `-O2 -DNDEBUG`; ASan/UBSan `-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all` | 24 | 240/240 each |
| macOS 27.0.1, Apple M3 Pro | Apple clang 15.0.0 | assertions `-g`; `-O2 -DNDEBUG`; UBSan `-g -O1 -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=undefined` under Guard Malloc (`libgmalloc`) | 24 | 240/240 each |

AddressSanitizer is not used on macOS: it does not start under Apple
clang 15 on that system.

## Limitations

- MC/DC was measured in one configuration, 2×8 with 2 KiB blocks, on
  macOS arm64, and separately with `RESOLVER1_WRITE_LG` 0. The 2×4 build
  and the other block sizes are covered by the test matrix, not by an
  MC/DC measurement.
- The Linux mapping code of `PagePool` (lines 42-81 of `pagepool.cpp`)
  is not compiled on macOS, so it is not instrumented; it has no decision
  with two or more conditions.
- `checkReads<24>` is compiled but not run. A condition of `checkReads`
  counts as covered when the instantiation for G = 16 or 32 shows a pair.
- The four conditions whose reachability was not established (145 C1,
  287 C3, 304 C2 and 329 C1) are not covered, and it is not known whether
  an input can cover them.
- No fault was injected: the defensive `std::logic_error` (803) and the
  failure paths of allocation are not exercised.
- The independence pairs are those computed by `llvm-cov` 20.1.8; the tool
  was not qualified independently. `mcdc_extract.py` checks only that its
  merged per-file counts equal those of `llvm-cov export`.
- Results apply to the source and environment identified above.

## Evidence

All files are under [evidence/mcdc/](evidence/mcdc/). The file
[INVENTORY.txt](evidence/mcdc/INVENTORY.txt) describes each one;
[SHA256SUMS](evidence/mcdc/SHA256SUMS) lists their hashes. Local paths
were removed as described in
[SANITIZATION.md](evidence/mcdc/SANITIZATION.md).

- [Test output](evidence/mcdc/2x8/tests.txt): the full suite, 240 tests,
  from the MC/DC build; the same run produced the report, `show` and
  `export` below.
- [MC/DC summary](evidence/mcdc/2x8/llvm-cov-report.txt):
  `llvm-cov report -show-mcdc-summary`, full suite, no filter.
- [Annotated MC/DC coverage](evidence/mcdc/2x8/llvm-cov-show.txt):
  `llvm-cov show -show-mcdc -show-branches=count`, same run.
- [MC/DC export](evidence/mcdc/2x8/llvm-cov-export.json):
  `llvm-cov export`, same run.
- [Conditions](evidence/mcdc/2x8/mcdc-conditions.tsv) and
  [records](evidence/mcdc/2x8/mcdc-records.tsv): extracted by
  `mcdc_extract.py` from the same run.
- [Vectors by test](evidence/mcdc/2x8/vectors-by-test.tsv): each test run
  alone, 240 runs, through `exactmain.cpp`.
- [Condition catalog](evidence/mcdc/2x8/mcdc-catalog.tsv): built by
  `build_catalog.py` from the two previous files and
  `classification.py`.
- [LG 0 measurement](evidence/mcdc/lg0/): the same files for the
  `RESOLVER1_WRITE_LG` 0 build, full suite, no filter.
- [Validation matrix](evidence/mcdc/matrix/): per configuration, the
  exact flags, the compiler output and the test output.
- [Diffs](evidence/mcdc/diffs/): the commit against `1233986b`, its code
  against `2a68fc9b`, and the production files against `f866c98d`.
- [Scripts](evidence/mcdc/scripts/) and
  [toolchain](evidence/mcdc/toolchain.txt).
