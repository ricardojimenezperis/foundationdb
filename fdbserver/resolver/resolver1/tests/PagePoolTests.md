# PagePool validation

## Scope

Validation of fixed-size block allocation and release within a
budget-limited pool of aligned 64 KiB pages: page and block layout, the requested budget and the bytes
actually allocated in whole pages, free-list allocation and release
order, the count of free blocks, exhaustion and recovery. Blocks are
handed out and returned as direct addresses.

The backing is selected by the environment variable
`RESOLVER1_PAGE_MODE`. Without it, or with `legacy`, the pool is one
`new Page[]` array; an unknown mode throws `std::invalid_argument`. The
modes `base`, `thp` and `hugetlb` map the pool with `mmap` on Linux when
the budget holds at least one whole page. With no whole page the pool is
an empty `new Page[]` array on every platform. On other platforms the
mapping code (lines 42-81) is not compiled, and a mapped mode with whole
pages throws `std::runtime_error`. The tests select every mode. The
coverage in this report was collected on macOS, so it does not include
the mapping code; on Linux, `PageModeBaseWithPages` maps a `base` pool in
the test matrix (see [Limitations](#limitations)).

History reclamation and key-storage lifetime management are outside
the scope of this component. Their use of the pool is validated in
[KeyStoreTests.md](KeyStoreTests.md).

## Source and environment

- Source commit: the tested files are identified by content; this report
  is committed together with them. SHA-256 (first 16 hex digits):

  | File | SHA-256 |
  |---|---|
  | `pagepool.cpp` | `66d0fa33d5e1b771` |
  | `pagepool.h` | `86f8dee90b69e43e` |
  | `common.h` | `a815af89662b3a73` |
  | `test.h` | `a9a025cd287fd82e` |
  | `testmain.cpp` | `f977afc8cc420126` |
  | `makefile` | `2a8173902bc33339` |
  | `tests/pagepooltests.cpp` | `eecfd20aba2b7979` |

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
./tests.exe /resolver/PagePool
```

Coverage collection with gcov, for `pagepool.cpp`:

```bash
rm -rf build/coverage && mkdir -p build/coverage
FLAGS="-I. -std=c++20 -Wall -Wextra -Wpedantic -O0 --coverage"
for f in pagepool keystore bnodestore tempconflictbtree testmain tests/pagepooltests tests/keystoretests tests/bnodestoretests tests/tempconflictbtreetests; do
	g++ $FLAGS -c $f.cpp -o build/coverage/$(basename $f).o
done
g++ --coverage build/coverage/*.o -o build/coverage/tests.exe
(cd build/coverage && ./tests.exe /resolver/PagePool)
gcov -b -c -o build/coverage/pagepool.o pagepool.cpp > build/coverage/pagepool-coverage.txt 2>&1
mv *.gcov build/coverage/
```

Coverage collection with llvm-cov, for `pagepool.cpp` and `pagepool.h`:

```bash
rm -rf build/coverage-llvm && mkdir -p build/coverage-llvm
g++ -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=. pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp testmain.cpp tests/*.cpp -o build/coverage-llvm/tests.exe
LLVM_PROFILE_FILE=build/coverage-llvm/pagepool.profraw build/coverage-llvm/tests.exe /resolver/PagePool
xcrun llvm-profdata merge -o build/coverage-llvm/pagepool.profdata build/coverage-llvm/pagepool.profraw
xcrun llvm-cov report build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/pagepool.profdata '-ignore-filename-regex=(keystore|bnodestore|tempconflictbtree|fdbcompat|ResolverBatchTypes|test)' > build/coverage-llvm/pagepool-llvm-report.txt
xcrun llvm-cov show build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/pagepool.profdata -show-branches=count '-ignore-filename-regex=(keystore|bnodestore|tempconflictbtree|fdbcompat|ResolverBatchTypes|test)' > build/coverage-llvm/pagepool-llvm-show.txt
```

Only the PagePool tests are run, so the KeyStore, BNodeStore and
TempConflictBTree tests, which also use the pool, do not contribute to
these figures. Each collection starts
from an empty directory, so counters from earlier runs are not merged
in. The `-ignore-filename-regex` option leaves only `pagepool.cpp` and
`pagepool.h` in the llvm-cov output.

`tests/coverage.sh` runs these commands for the four components, after
the full suite whose output is the test evidence. Run it from the
project root:

```bash
bash tests/coverage.sh
```

## Test cases

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/PagePool/AllocationOrder` | Initial allocations return consecutive addresses in page and offset order; exhaustion returns `nullptr`. | PASS |
| `/resolver/PagePool/ReuseReleasedBlock` | A released block is allocated again at the same address. | PASS |
| `/resolver/PagePool/PageAlignment` | With one block per page, block addresses are non-null and aligned to 64 KiB. | PASS |
| `/resolver/PagePool/BlocksDoNotOverlap` | Writing allocated blocks preserves the contents of other allocated blocks. | PASS |
| `/resolver/PagePool/EmptyPool` | Budgets of `0`, `1` and `PageSize - 1` yield no pages; repeated allocation keeps returning `nullptr`. | PASS |
| `/resolver/PagePool/PartialPageTail` | With a block size that does not divide the page, only whole blocks are handed out, at the expected address of each page, and the page tail is never used. | PASS |
| `/resolver/PagePool/MinimumBlockSize` | Block size `sizeof(FreeBlock)` is accepted; all `PageSize / 8` blocks are allocated at consecutive addresses. | PASS |
| `/resolver/PagePool/MaximumBlockSize` | Block size `PageSize` is accepted; each page holds exactly one block, at the page-aligned start of the page. | PASS |
| `/resolver/PagePool/BudgetRoundsDownToWholePages` | A budget that is not a page multiple is rounded down to whole pages: two blocks, one page apart, then `nullptr`. | PASS |
| `/resolver/PagePool/BudgetAndAllocatedBytes` | For budgets `0`, `PageSize - 1`, `PageSize`, `3 * PageSize - 1` and `3 * PageSize`, `budget()` returns the requested budget, `allocatedBytes()` returns it rounded down to whole pages, and the number of allocatable blocks times `blockSize()` equals `allocatedBytes()`. | PASS |
| `/resolver/PagePool/ReleaseAfterExhaustion` | Releasing a block into an exhausted pool makes it allocatable again at the same address, and only that block. | PASS |
| `/resolver/PagePool/ReleaseIsLifo` | Released blocks are reallocated in last-in, first-out order, compared by address. | PASS |
| `/resolver/PagePool/ReleaseBlockFromAnotherPage` | Blocks from different pages, one page apart, can be released and are reused in last-in, first-out order. | PASS |
| `/resolver/PagePool/AvailableBlocks` | With a budget of two pages and 100 bytes and 1000-byte blocks, `availableBlocks()` starts at the number of blocks in the two whole pages, goes down by one with each allocation, stays at 0 when allocation fails, and goes up by one with each release. | PASS |
| `/resolver/PagePool/AvailableBlocksOfEmptyPool` | A pool with a budget of `PageSize - 1` reports 0 available blocks, before and after a failed allocation. | PASS |
| `/resolver/PagePool/PageModeLegacyByDefaultAndByName` | With `RESOLVER1_PAGE_MODE` unset, and set to `legacy`, a pool of two pages has the `Legacy` backing, `backingBytes()` of two pages and every block available, and allocation succeeds. The variable is restored to its previous value after the test, as in the other three `PageMode` tests. | PASS |
| `/resolver/PagePool/PageModeUnknownThrows` | With `RESOLVER1_PAGE_MODE=huge` the constructor throws `std::invalid_argument`. | PASS |
| `/resolver/PagePool/PageModeExplicitWithoutPages` | With `base`, `thp` and `hugetlb`, and budgets of `0` and `PageSize - 1`, the pool reports the selected backing, `backingBytes()` 0 and 0 available blocks, and allocation returns `nullptr`, on every platform. | PASS |
| `/resolver/PagePool/PageModeBaseWithPages` | With `base` and a budget of two pages: on Linux the mapping starts at a 2 MiB boundary, `backingBytes()` is the budget rounded up to whole 2 MiB, every block is available and allocation succeeds; on other platforms the constructor throws `std::runtime_error`. The coverage run on macOS executes the second form; the Linux matrix executes the first. | PASS |

19 PagePool tests passed, 0 failed. The full run, including the 41
KeyStore tests, the 48 BNodeStore tests and the 132 TempConflictBTree
tests, reports 240 passed and 0 failed.

The constructor requires a block size of at least `sizeof(FreeBlock)`,
at most `PageSize`, and a multiple of `alignof(FreeBlock)`. It checks
this precondition with `assert`, which terminates the process when it
fails and is removed when `NDEBUG` is defined. The assertion builds do not
define `NDEBUG`. Block sizes that do not meet the precondition are
outside the interface and are not tested. `MinimumBlockSize` and
`MaximumBlockSize` use the smallest and largest accepted sizes; they
assume `sizeof(FreeBlock) == 8` and `alignof(FreeBlock) == 8`, as
verified on the tested platform.

## Coverage

Coverage was collected for `pagepool.cpp` with gcov, and for
`pagepool.cpp` and `pagepool.h` with llvm-cov, running only the
PagePool tests.

gcov, `pagepool.cpp`:

| Metric | Result |
|---|---|
| Lines executed | 98.00% of 50 |
| Branches executed | 100.00% of 34 |
| Taken at least once | 79.41% of 34 |

llvm-cov source-based coverage:

| File | Regions | Functions | Lines | Branches |
|---|---|---|---|---|
| `pagepool.cpp` | 97.22% of 36 | 100.00% of 4 | 100.00% of 52 | 95.45% of 22 |
| `pagepool.h` | 80.00% of 10 | 85.71% of 7 | 85.71% of 7 | 50.00% of 2 |

The classification in this section was made on the tree identified in
[Source and environment](#source-and-environment), from the Apple clang 15
evidence of the same run.

These are gcov and llvm-cov metrics. They are not a measurement of
MC/DC, and full coverage does not show that the code is free of
errors. "Taken at least once" does not mean that every decision was
taken in both directions. Apple clang 15 provides no condition
coverage equivalent to GCC's `-fcondition-coverage`. MC/DC was measured
separately with LLVM/clang 20 and is reported in
[MCDCCoverage.md](MCDCCoverage.md): `pagepool.cpp` has 7 instrumented
conditions, 4 of them with an independence pair; the 3 without one are
the conditions of the `assert` on line 20.

The two tools count different things. gcov counts branches between
the basic blocks of the generated code, including branches the
compiler adds. llvm-cov counts the true and false outcomes of each
condition written in the source, so its 22 branches are the 11
conditions of `pagepool.cpp` outside the `assert`. llvm-cov reports no
branch for the `assert` in the constructor, whose condition comes from
a macro in a system header; gcov does report it. The Linux code of the
constructor and the destructor (lines 42-81 and 98-100) is not compiled
on the tested platform, so neither tool counts it.

The functions defined in `pagepool.h` are inline. They are measured
with llvm-cov only. With `--coverage`, each translation unit keeps its
own counters for an inline function, and the linker keeps one copy.
Reading `pagepool.h` through one object file can therefore report a
function as not executed although the tests call it. llvm-cov merges
the counters of all copies. Its execution counts are `blockSize` 7,
`budget` 5, `allocatedBytes` 18, `availableBlocks` 272, `backing` 8 and
`backingBytes` 8. `backingAddress` is not executed: only the Linux form
of `PageModeBaseWithPages` calls it. The condition of `backingBytes`
(`pagepool.h:49`, `mappedBytes_ == 0`) is true on every execution,
because only the Linux mapping code sets `mappedBytes_`.

### Decisions

Outside the `assert`, `pagepool.cpp` has two compound decisions, both in
the selection of the backing, and seven single conditions: the modes
compared on lines 30, 32 and 34, the two constructor loops (lines 87 and
89), the mapped backing in the destructor (line 97) and the empty free
list in `allocateBlock` (line 107). llvm-cov reports both outcomes
covered for every condition except line 97, which is false on every
execution (see below): `pagepool.cpp:30` (true 3, false 5),
`pagepool.cpp:32` (true 2, false 3), `pagepool.cpp:34` (true 2, false 1),
`pagepool.cpp:87` (true 33, false 29), `pagepool.cpp:89` (true 9.51k,
false 33) and `pagepool.cpp:107` (true 29, false 9.17k).

`mode == nullptr || std::strcmp(mode, "legacy") == 0` (line 28):

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Variable not set | `RESOLVER1_PAGE_MODE` unset | Not evaluated | Legacy | Every test outside the `PageMode` tests, and `PageModeLegacyByDefaultAndByName` |
| Mode `legacy` | `RESOLVER1_PAGE_MODE=legacy` | Variable set | Legacy | `PageModeLegacyByDefaultAndByName` |
| Another mode | `base`, `thp`, `hugetlb` or `huge` | Variable set | Not legacy | `PageModeUnknownThrows`, `PageModeExplicitWithoutPages`, `PageModeBaseWithPages` |

`backing_ == Backing::Legacy || numPages_ == 0` (line 39):

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| Legacy backing | `RESOLVER1_PAGE_MODE` unset or `legacy` | Not evaluated | `new Page[]` | Every test outside the `PageMode` tests, and `PageModeLegacyByDefaultAndByName` |
| No whole page | A mapped mode and a budget below `PageSize` | Not legacy | `new Page[]` | `PageModeExplicitWithoutPages` |
| Mapped backing with pages | A mapped mode and a budget of one page or more | Not legacy | Mapping, or the `std::runtime_error` on this platform | `PageModeBaseWithPages` |

llvm-cov counts: `pagepool.cpp:28` `mode == nullptr` true 22, false 9;
`strcmp(mode, "legacy") == 0` true 1, false 8. `pagepool.cpp:39`
`backing_ == Backing::Legacy` true 23, false 7; `numPages_ == 0` true 6,
false 1. In each table, the first and last rows differ only in the first
condition and the second and last rows only in the second, with
different decisions; the MC/DC measurement in
[MCDCCoverage.md](MCDCCoverage.md) reports these pairs for both
decisions.

`releaseBlock` has the precondition that the address must be the start
of a block allocated by this pool and not already released. The tests
release only addresses returned by `allocateBlock`. Addresses that do
not meet the precondition are outside the interface and are not tested.

### Uncovered generated branches

gcov reports 7 untaken branches: none never executed and 7 taken 0
times. Each was matched to its edge in the LLVM IR that Apple clang 15
generates for `pagepool.cpp` at `-O0` with `-g`: llvm-cov gcov lists the
out-edges of a block at the last source line of the block, and on each
of the 13 lines with branch items the number of items equals the number
of edges of the conditional branches and `invoke` instructions whose
block ends on that line.

**Precondition violation (3 items).** The `assert` in the constructor
evaluates its three conditions with short-circuit branches. Each
untaken item is the path to `__assert_rtn` taken when one of them is
false, which happens only when the block-size precondition described in
[Test cases](#test-cases) does not hold.

| Location | gcov items |
|---|---|
| `pagepool.cpp:20` | branches 1, 2 and 5 |

**Allocation failure not injected (2 items).** Possible failures that
no test injects. Each is the exceptional edge of an `invoke` of an
exception constructor, taken only if constructing the exception object
itself throws, for example `std::bad_alloc` while copying its message.
The normal edge of both is taken.

| Location | gcov item | Outcome |
|---|---|---|
| `pagepool.cpp:37` | branch 1 | The `std::invalid_argument` constructor for an unknown mode throws |
| `pagepool.cpp:83` | branch 1 | The `std::runtime_error` constructor for a mapped mode on a platform other than Linux throws |

**Unreachable on the tested platform (2 items).** They do not
correspond to an outcome that can occur in this build.

| Location | gcov item | Origin and reason |
|---|---|---|
| `pagepool.cpp:97` | branch 0 | Mapped pool in the destructor. Only the Linux code sets `mappedBytes_`, and it is not compiled here. |
| `pagepool.cpp:102` | branch 1 | Null check before the call to the array `operator delete` in `delete[] pages_`. The throwing form of `new[]` used by the constructor never returns null, even for zero pages, and the constructor either assigns its result or throws before the object exists. |

## Limitations

- The constructor sets no upper limit on the budget. A budget that the
  system cannot satisfy is reported by `new Page[numPages_]` with
  `std::bad_alloc`; this was not tested.
- No allocation-failure injection was performed.
- The mapping code of the `base`, `thp` and `hugetlb` backings (lines
  42-81) is compiled only on Linux and is outside the coverage above. In
  the Linux matrix, `PageModeBaseWithPages` maps a `base` pool of two
  pages in every configuration. `thp` and `hugetlb` with whole pages, and
  the failures of `mmap` and `madvise`, are not tested.
- The full suite was run on the tree identified above in two matrices of
  24 configurations each: write tasks 2×8 and 2×4, blocks of 1, 2, 4 and
  8 KiB, and assertions, `-O2 -DNDEBUG` and sanitizers (ASan/UBSan on
  Linux with clang 19.1.5 and libc++; UBSan with Guard Malloc on macOS
  with Apple clang 15.0.0). All 48 configurations passed 240 of 240
  tests, with no compiler warning and no sanitizer report. The runs are
  in [evidence/mcdc/matrix/](evidence/mcdc/matrix/).
- Releasing an invalid or already-free block is undefined behaviour in
  this interface and is not tested.
- Condition coverage is not available with Apple clang 15. MC/DC was
  measured with LLVM/clang 20 and is reported in
  [MCDCCoverage.md](MCDCCoverage.md).
- Results apply to the source and environment identified above.

## Evidence

- [Test output](evidence/pagepool-tests.txt): output of `make -B test`
  for the full suite (240 tests, PagePool, KeyStore, BNodeStore and
  TempConflictBTree). It is the same run as `keystore-tests.txt`,
  `bnodestore-tests.txt` and `tempconflictbtree-tests.txt`.
- [Coverage summary](evidence/pagepool-coverage.txt): gcov output after
  running only `./tests.exe /resolver/PagePool`. The filter does not
  appear in the gcov output; it is recorded in
  [Reproduction](#reproduction).
- Annotated coverage, `evidence/pagepool.cpp.gcov`: from the same gcov
  run.
- [llvm-cov summary](evidence/pagepool-llvm-report.txt): llvm-cov report
  after running only `build/coverage-llvm/tests.exe /resolver/PagePool`.
  The filter does not appear in the output.
- llvm-cov annotated coverage, `evidence/pagepool-llvm-show.txt`:
  `pagepool.cpp` and `pagepool.h` with line and branch counts, from the
  same llvm-cov run.

The two annotated reports are generated by `tests/coverage.sh` and are not
tracked; running it on the source identified above regenerates the ones
this report describes.
