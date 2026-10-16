# PagePool validation

## Scope

Validation of fixed-size block allocation and release within a
budget-limited pool of aligned 64 KiB pages: page and block layout, the requested budget and the bytes
actually allocated in whole pages, free-list allocation and release
order, exhaustion and recovery. Blocks are handed out and returned as
direct addresses.

History reclamation and key-storage lifetime management are outside
the scope of this component. Their use of the pool is validated in
[KeyStoreTests.md](KeyStoreTests.md).

## Source and environment

- Source commit: not under version control; tested files identified by
  SHA-256 (first 16 hex digits):

  | File | SHA-256 |
  |---|---|
  | `pagepool.cpp` | `7506a3e1f65b263a` |
  | `pagepool.h` | `b6549ed8ffe91ea8` |
  | `common.h` | `0a0b864acb86c330` |
  | `test.h` | `a9a025cd287fd82e` |
  | `testmain.cpp` | `f977afc8cc420126` |
  | `makefile` | `246b6c0baf2b6a99` |
  | `tests/pagepooltests.cpp` | `924bc131b8ce98f3` |

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
for f in pagepool keystore bnodestore testmain tests/pagepooltests tests/keystoretests tests/bnodestoretests; do
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
g++ -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=. pagepool.cpp keystore.cpp bnodestore.cpp testmain.cpp tests/*.cpp -o build/coverage-llvm/tests.exe
LLVM_PROFILE_FILE=build/coverage-llvm/pagepool.profraw build/coverage-llvm/tests.exe /resolver/PagePool
xcrun llvm-profdata merge -o build/coverage-llvm/pagepool.profdata build/coverage-llvm/pagepool.profraw
xcrun llvm-cov report build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/pagepool.profdata '-ignore-filename-regex=(keystore|bnodestore|fdbcompat|test)' > build/coverage-llvm/pagepool-llvm-report.txt
xcrun llvm-cov show build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/pagepool.profdata -show-branches=count '-ignore-filename-regex=(keystore|bnodestore|fdbcompat|test)' > build/coverage-llvm/pagepool-llvm-show.txt
```

Only the PagePool tests are run, so the KeyStore and BNodeStore tests,
which also use the pool, do not contribute to these figures. Each collection starts
from an empty directory, so counters from earlier runs are not merged
in. The `-ignore-filename-regex` option leaves only `pagepool.cpp` and
`pagepool.h` in the llvm-cov output.

`tests/coverage.sh` runs these commands for the three components, after
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

13 PagePool tests passed, 0 failed. The full run, including the 35
KeyStore tests and the 41 BNodeStore tests, reports 89 passed and 0
failed.

The constructor requires a block size of at least `sizeof(FreeBlock)`,
at most `PageSize`, and a multiple of `alignof(FreeBlock)`. It checks
this precondition with `assert`, which terminates the process when it
fails and is removed when `NDEBUG` is defined. The tested builds do not
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
| Lines executed | 100.00% of 29 |
| Branches executed | 100.00% of 14 |
| Taken at least once | 71.43% of 14 |

llvm-cov source-based coverage:

| File | Regions | Functions | Lines | Branches |
|---|---|---|---|---|
| `pagepool.cpp` | 100.00% of 12 | 100.00% of 4 | 100.00% of 30 | 100.00% of 6 |
| `pagepool.h` | 100.00% of 3 | 100.00% of 3 | 100.00% of 3 | no branches |

These are gcov and llvm-cov metrics. They are not a measurement of
MC/DC, and full coverage does not show that the code is free of
errors. "Taken at least once" does not mean that every decision was
taken in both directions. Apple clang 15 provides no condition
coverage equivalent to GCC's `-fcondition-coverage`.

The two tools count different things. gcov counts branches between
the basic blocks of the generated code, including branches the
compiler adds. llvm-cov counts the true and false outcomes of each
condition written in the source, so its 6 branches are the 3
conditions of `pagepool.cpp` outside the `assert`, each taken in both
directions. llvm-cov reports no branch for the `assert` in the
constructor, whose condition comes from a macro in a system header;
gcov does report it.

The functions defined in `pagepool.h` (`blockSize`, `budget` and
`allocatedBytes`) are inline. They
are measured with llvm-cov only. With `--coverage`, each translation
unit keeps its own counters for an inline function, and the linker
keeps one copy. Reading `pagepool.h` through one object file can
therefore report a function as not executed although the tests call
it. llvm-cov merges the counters of all copies. Its execution counts
are `blockSize` 7, `budget` 5 and `allocatedBytes` 10.

### Decisions

The decisions in `pagepool.cpp` outside the `assert` have a single
condition: the two constructor loops and the empty free list in
`allocateBlock`. llvm-cov reports both outcomes covered for each of
them: `pagepool.cpp:19` (true 27, false 19), `pagepool.cpp:21` (true
9.13k, false 27) and `pagepool.cpp:33` (true 21, false 9.04k).

`releaseBlock` has the precondition that the address must be the start
of a block allocated by this pool and not already released. The tests
release only addresses returned by `allocateBlock`. Addresses that do
not meet the precondition are outside the interface and are not tested.

### Uncovered generated branches

gcov reports 4 untaken branches. Their origin was checked in the LLVM
IR that Apple clang 15 generates for `pagepool.cpp` at `-O0`.

**Precondition violation.** The `assert` in the constructor evaluates
its three conditions with short-circuit branches. Each untaken item is
the path to `__assert_rtn` taken when one of them is false, which
happens only when the block-size precondition described in
[Test cases](#test-cases) does not hold.

| Location | gcov item |
|---|---|
| `pagepool.cpp:10` | branches 1, 2 and 4 |

**Unreachable.** It does not correspond to a condition written in the
source.

| Location | gcov item | Origin and reason |
|---|---|---|
| `pagepool.cpp:29` | branch 1 | Null check before the call to the array `operator delete` in `delete[] pages_`. The throwing form of `new[]` used by the constructor never returns null, even for zero pages, and the constructor either assigns its result or throws before the object exists. |

## Limitations

- The constructor sets no upper limit on the budget. A budget that the
  system cannot satisfy is reported by `new Page[numPages_]` with
  `std::bad_alloc`; this was not tested.
- No allocation-failure injection was performed.
- A build with `NDEBUG` defined removes the block-size check; such a
  build was not tested.
- Releasing an invalid or already-free block is undefined behaviour in
  this interface and is not tested.
- No condition coverage was measured: the toolchain does not provide
  it.
- Results apply to the source and environment identified above.

## Evidence

- [Test output](evidence/pagepool-tests.txt): output of `make -B test`
  for the full suite (89 tests, PagePool, KeyStore and BNodeStore). It
  is the same run as `keystore-tests.txt` and `bnodestore-tests.txt`.
- [Coverage summary](evidence/pagepool-coverage.txt): gcov output after
  running only `./tests.exe /resolver/PagePool`. The filter does not
  appear in the gcov output; it is recorded in
  [Reproduction](#reproduction).
- [Annotated coverage](evidence/pagepool.cpp.gcov): from the same gcov
  run.
- [llvm-cov summary](evidence/pagepool-llvm-report.txt): llvm-cov report
  after running only `build/coverage-llvm/tests.exe /resolver/PagePool`.
  The filter does not appear in the output.
- [llvm-cov annotated coverage](evidence/pagepool-llvm-show.txt):
  `pagepool.cpp` and `pagepool.h` with line and branch counts, from the
  same llvm-cov run.
