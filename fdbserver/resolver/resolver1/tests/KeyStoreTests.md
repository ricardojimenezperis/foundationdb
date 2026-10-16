# KeyStore validation

## Scope

Validation of variable-length key storage in blocks taken from a shared
`PagePool`: key append within one block and across linked blocks, the
empty key, reservation of the first block on demand rather than at
construction, all-or-nothing reservation of the blocks a key needs,
including a new open block,
recovery after pool exhaustion, oldest-first reclamation, `maxCV` tracking,
generation-based validity of key references, and byte-wise lexicographic
comparison of stored keys with each other and of an external contiguous
key (`StringRef`) with a stored key.

Outside the scope of this component: `PagePool` itself (see
[PagePoolTests.md](PagePoolTests.md)), the choice of the commit-version
cutoff for reclamation, which belongs to the caller, the B+ tree that
holds key references, and concurrent access. `fdbcompat.h` provides
stand-ins for FoundationDB types (`StringRef`) so that the component
builds without `flow`; it is not measured as part of this component. The rule that no
reclamation may occur while key bytes are being accessed is a caller
contract and is not tested here.

## Source and environment

- Source commit: not under version control; tested files identified by
  SHA-256 (first 16 hex digits):

  | File | SHA-256 |
  |---|---|
  | `keystore.cpp` | `f294adf3d5a145bc` |
  | `keystore.h` | `e5de2129d650f0a4` |
  | `fdbcompat.h` | `788cc62b6648bf6d` |
  | `common.h` | `0a0b864acb86c330` |
  | `pagepool.cpp` | `7506a3e1f65b263a` |
  | `pagepool.h` | `b6549ed8ffe91ea8` |
  | `test.h` | `a9a025cd287fd82e` |
  | `testmain.cpp` | `f977afc8cc420126` |
  | `makefile` | `246b6c0baf2b6a99` |
  | `tests/keystoretests.cpp` | `51ab3743efb7d13a` |
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
./tests.exe /resolver/KeyStore
```

Coverage collection with gcov:

```bash
rm -rf build/coverage && mkdir -p build/coverage
FLAGS="-I. -std=c++20 -Wall -Wextra -Wpedantic -O0 --coverage"
for f in pagepool keystore bnodestore testmain tests/pagepooltests tests/keystoretests tests/bnodestoretests; do
	g++ $FLAGS -c $f.cpp -o build/coverage/$(basename $f).o
done
g++ --coverage build/coverage/*.o -o build/coverage/tests.exe
(cd build/coverage && ./tests.exe /resolver/KeyStore)
gcov -b -c -o build/coverage/keystore.o keystore.cpp > build/coverage/keystore-coverage.txt 2>&1
mv *.gcov build/coverage/
```

Coverage collection with llvm-cov:

```bash
rm -rf build/coverage-llvm && mkdir -p build/coverage-llvm
g++ -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=. pagepool.cpp keystore.cpp bnodestore.cpp testmain.cpp tests/*.cpp -o build/coverage-llvm/tests.exe
LLVM_PROFILE_FILE=build/coverage-llvm/keystore.profraw build/coverage-llvm/tests.exe /resolver/KeyStore
xcrun llvm-profdata merge -o build/coverage-llvm/keystore.profdata build/coverage-llvm/keystore.profraw
xcrun llvm-cov report build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/keystore.profdata '-ignore-filename-regex=(pagepool|bnodestore|fdbcompat|test)' > build/coverage-llvm/keystore-llvm-report.txt
xcrun llvm-cov show build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/keystore.profdata -show-branches=count '-ignore-filename-regex=(pagepool|bnodestore|fdbcompat|test)' > build/coverage-llvm/keystore-llvm-show.txt
```

Only the KeyStore tests are run. Each collection starts from an empty
directory, so counters from earlier runs are not merged in. The
`-ignore-filename-regex` option removes the `PagePool`, `BNodeStore`,
`fdbcompat.h` and test sources from the llvm-cov output. `keystore.h` and `common.h`
contain no function bodies, so `keystore.cpp` is the only KeyStore file with
instrumented code.

`tests/coverage.sh` runs these commands for the three components, after
the full suite whose output is the test evidence. Run it from the
project root:

```bash
bash tests/coverage.sh
```

## Test cases

Unless stated otherwise, tests use 64-byte blocks: a 24-byte
`KeyBlockHeader` followed by 40 payload bytes.

| Test case | Property checked | Result |
|---|---|---|
| `/resolver/KeyStore/AppendAfterReclaimingAllBlocks` | With one block in the pool: after filling it and reclaiming it, a new key starts a new list at offset 24 with a correct header, and its block can be reclaimed and returned to the pool. | PASS |
| `/resolver/KeyStore/AppendAfterReclaimingOldestBlock` | With every block full and only the oldest reclaimed, a new key's block is linked after the last full block; reclamation follows the list to it and both blocks return to the pool. | PASS |
| `/resolver/KeyStore/DestructorReturnsAllBlocks` | After storing single- and multi-block keys, destroying the store returns every block to the pool. | PASS |
| `/resolver/KeyStore/ReusedBlockDoesNotReviveOldReferences` | When a reclaimed block is reused for a new key at the same block address and offset, the new key gets a higher generation, the old reference stays invalid, and the new one is valid with the new bytes. | PASS |
| `/resolver/KeyStore/FailedAppendKeepsExistingKeys` | A multi-block append that fails after an earlier key leaves that key's bytes, validity and `maxCV` unchanged, and the next key follows it in the same block. | PASS |
| `/resolver/KeyStore/OutOfOrderCV` | Keys with decreasing versions in one block keep the block's `maxCV` at the maximum (20, then 10 gives 20). | PASS |
| `/resolver/KeyStore/EqualCVBlocksReclaimedIndividually` | Two blocks with the same version are reclaimed one at a time: reclaiming the first invalidates only its keys. | PASS |
| `/resolver/KeyStore/EmptyPool` | A store is constructed over an empty pool; `oldestCV` and `reclaimOldest` return `std::nullopt`, a non-empty key returns `std::nullopt`, and the empty key is accepted and valid. | PASS |
| `/resolver/KeyStore/BlockSize` | `blockSize()` reports the pool's block size. | PASS |
| `/resolver/KeyStore/EmptyKey` | The empty key is accepted without storage: length 0, `keyStart` and `keyBlock` return `nullptr`, it is valid, it consumes no bytes and records no version (`oldestCV` stays empty). | PASS |
| `/resolver/KeyStore/EmptyKeyWithoutFreeBlocks` | The empty key is accepted with the pool exhausted and no open block, and stays valid after every block is reclaimed. | PASS |
| `/resolver/KeyStore/SingleBlockKeys` | Consecutive keys are stored back to back in one block, with correct offsets, bytes, generation and `maxCV`; `keyStart` and `keyBlock` resolve the key's location. | PASS |
| `/resolver/KeyStore/MultiBlockKey` | A key starting mid-block spans four blocks (25 + 40 + 40 + 15 bytes) and reads back intact; every block records the key's version, continuation blocks are newer than the first, and the next key continues in the last block. | PASS |
| `/resolver/KeyStore/KeyFillsBlockExactly` | A key ending exactly at a block boundary is followed by a fresh open block, where the next key starts at offset 24 with a newer generation. | PASS |
| `/resolver/KeyStore/ReservationFailsOnFirstExtraBlock` | Without an open block and with one free block, a key needing two blocks fails and takes no block from the pool; the next key opens a block and starts at offset 24. | PASS |
| `/resolver/KeyStore/ReservationFailsOnLaterExtraBlock` | Without an open block and with two free blocks, a key needing three blocks fails and the two blocks already reserved return to the pool; the next key opens a block and starts at offset 24. | PASS |
| `/resolver/KeyStore/OldestCV` | `oldestCV` and `reclaimOldest` return `std::nullopt` before any key bytes are stored; `oldestCV` then tracks the maximum version in the oldest block, follows reclamation, and is empty when no blocks remain. | PASS |
| `/resolver/KeyStore/IsValid` | Keys are valid until their block is reclaimed, including keys in the first block; reclaiming one block does not invalidate keys in later blocks. | PASS |
| `/resolver/KeyStore/IsValidMultiBlockKey` | A key spanning three blocks becomes invalid when its first block is reclaimed. | PASS |
| `/resolver/KeyStore/CompareKeysEqual` | A key compared with itself, and two equal keys stored at different places, compare equal. | PASS |
| `/resolver/KeyStore/CompareKeysEmpty` | Two empty keys compare equal, and the empty key precedes every other key, including `{0x00}`. | PASS |
| `/resolver/KeyStore/CompareKeysPrefix` | A proper prefix precedes the longer key, whatever the extra bytes: `"ab"` < `"ab\0"` < `"abc"`. | PASS |
| `/resolver/KeyStore/CompareKeysFirstDifferenceDecides` | The first differing byte decides, at the first, a middle or the last position, even when every later byte of the greater key is lower. | PASS |
| `/resolver/KeyStore/CompareKeysUnsignedBytes` | Bytes compare as unsigned values: `0x00` < `0x7F` < `0x80` < `0xFF`. | PASS |
| `/resolver/KeyStore/CompareKeysMultiBlock` | Keys spanning four blocks, stored at different offsets so that their fragment boundaries do not line up, compare equal; a difference of one in each fragment, including the bytes on either side of a block boundary, is ordered in the direction of the difference; a multi-block proper prefix precedes the longer key. | PASS |
| `/resolver/KeyStore/CompareKeysAgainstReference` | 80 keys derived from one 160-byte base key (prefixes of every length, 45 of them with one byte replaced by an extreme value) are compared in all 6,400 ordered pairs with the lexicographic order of `std::vector<std::byte>`. | PASS |

Every comparison test checks the sign of the result, not its value, and
checks both argument orders. Each pair of keys is compared twice: the two
stored keys with each other, and the contiguous bytes of each key, as a
`StringRef`, with the other stored key. `CompareKeysAgainstReference`
also compares every external key with every stored key, 6,400 more
comparisons. The counts of keys with a replaced byte in
`CompareKeysAgainstReference` come from replaying its random decisions
with the same seed, not from the test output.

26 KeyStore tests passed, 0 failed. The full run, including the 13
PagePool tests and the 41 BNodeStore tests, reports 80 passed and 0
failed.

The constructor requires a pool block size larger than
`sizeof(KeyBlockHeader)`. It checks this precondition with `assert`,
which terminates the process when it fails and is removed when `NDEBUG`
is defined. The tested builds do not define `NDEBUG`. A pool that does
not meet the precondition is outside the interface and is not tested.

The helper that reads keys back asserts that each block in the chain is
non-null, so a broken chain fails the test instead of reading invalid
memory.

## Coverage

Coverage was collected for `keystore.cpp`, running only the KeyStore
tests.

gcov:

| Metric | Result |
|---|---|
| Lines executed | 100.00% of 190 |
| Branches executed | 100.00% of 96 |
| Taken at least once | 87.50% of 96 |

llvm-cov source-based coverage:

| Metric | Result |
|---|---|
| Regions | 100.00% of 109 |
| Functions | 100.00% of 11 |
| Lines | 100.00% of 209 |
| Branches | 100.00% of 76 |

These are gcov and llvm-cov metrics. They are not a measurement of
MC/DC, and full coverage does not show that the code is free of
errors. "Taken at least once" does not mean that every decision was
taken in both directions. Apple clang 15 provides no condition
coverage equivalent to GCC's `-fcondition-coverage`.

gcov counts branches between the basic blocks of the generated code,
including the exception edges the compiler adds. llvm-cov counts the
true and false outcomes of each condition written in the source: its 76
branches are the 38 conditions of `keystore.cpp`, each taken in both
directions. llvm-cov reports no branch for the `assert` in the
constructor, whose condition comes from a macro in a system header;
gcov does report it.

### Independent condition checks

There are seven compound decisions: one in `isValid`, three in the
comparison of two stored keys and three in the comparison of an external
key with a stored key.

In `isValid`:
`ref.keyLength == 0 || ref.blockGen > lastReclaimedKeyGeneration_`.
Each condition decides the outcome on its own in these cases:

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| Empty key | `keyLength == 0` | Not evaluated | Valid | `EmptyKeyWithoutFreeBlocks` (after reclaiming every block) |
| Block not yet reclaimed | `keyLength > 0`, generation above the last reclaimed | False | Valid | `IsValid` |
| Block reclaimed | `keyLength > 0`, generation at or below the last reclaimed | False | Invalid | `IsValid` |

llvm-cov reports both conditions taken in both directions:
`keystore.cpp:166` (true 4, false 13) and `keystore.cpp:167` (true 7,
false 6).

In `compareKeys(const KeyRef&, const KeyRef&)`, the loop runs while both
keys have bytes left:
`rem1 != 0 && rem2 != 0` (line 189).

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| First key exhausted | `rem1 == 0` | Not evaluated | Stop | `CompareKeysPrefix`, shorter key first |
| Second key exhausted | `rem2 == 0` | `rem1 != 0` | Stop | `CompareKeysPrefix`, shorter key second |
| Both keys have bytes left | `rem1 != 0`, `rem2 != 0` | True | Continue | Every comparison of non-empty keys |

Inside the loop, each key moves to its next block when its current
fragment is used up and bytes remain: `bytes1 == 0 && rem1 != 0`
(line 201), and the same for the second key (line 206).

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| Fragment not used up | `bytes1 > 0` | Not evaluated | Stay | Every comparison |
| Key ended with its fragment | `bytes1 == 0`, `rem1 == 0` | True | Stay | Equal keys, and a key that is a prefix of the other |
| Key continues in the next block | `bytes1 == 0`, `rem1 != 0` | True | Move | `CompareKeysMultiBlock`, `CompareKeysAgainstReference` |

llvm-cov reports every condition taken in both directions:

| Location | Condition | True | False |
|---|---|---|---|
| `keystore.cpp:189` | `rem1 != 0` | 18.9k | 1.07k |
| `keystore.cpp:189` | `rem2 != 0` | 17.9k | 981 |
| `keystore.cpp:201` | `bytes1 == 0` | 7.00k | 6.57k |
| `keystore.cpp:201` | `rem1 != 0` | 5.93k | 1.07k |
| `keystore.cpp:206` | `bytes2 == 0` | 7.00k | 6.57k |
| `keystore.cpp:206` | `rem2 != 0` | 5.93k | 1.07k |

In `compareKeys(const StringRef&, const KeyRef&)`, the external key is
contiguous, so only the stored key moves to its next block. The loop
runs while both keys have bytes left, `rem1 != 0 && rem2 != 0`
(line 230), with the same cases as the table above. The stored key moves
to its next block when its fragment is used up and bytes remain,
`bytes2 == 0 && rem2 != 0` (line 241). After the loop, the keys are
equal when both are exhausted, `rem1 == 0 && rem2 == 0` (line 247):

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| External key not exhausted | `rem1 != 0` | Not evaluated | Not equal | A stored key that is a proper prefix of the external key |
| Stored key not exhausted | `rem1 == 0`, `rem2 != 0` | True | Not equal | An external key that is a proper prefix of the stored key |
| Both exhausted | `rem1 == 0`, `rem2 == 0` | True | Equal | `CompareKeysEqual`, `CompareKeysMultiBlock` |

llvm-cov reports every condition taken in both directions:

| Location | Condition | True | False |
|---|---|---|---|
| `keystore.cpp:230` | `rem1 != 0` | 13.3k | 1.07k |
| `keystore.cpp:230` | `rem2 != 0` | 12.3k | 981 |
| `keystore.cpp:241` | `bytes2 == 0` | 7.00k | 960 |
| `keystore.cpp:241` | `rem2 != 0` | 5.93k | 1.07k |
| `keystore.cpp:247` | `rem1 == 0` | 1.07k | 981 |
| `keystore.cpp:247` | `rem2 == 0` | 94 | 981 |

The other decisions in `keystore.cpp` have a single condition, and
llvm-cov reports both outcomes covered for each of them.

The tests' block arithmetic assumes `sizeof(KeyBlockHeader) == 24`,
which holds on the tested platform: keys appended to a fresh block
start at offset 24.

### Uncovered generated branches

gcov reports 12 untaken branches. Each was compared with the LLVM IR
that Apple clang 15 generates for `keystore.cpp` at `-O0`, with and
without `--coverage`. Line numbers refer to `keystore.cpp`.

**Precondition violation.** The failure path of the `assert` in the
constructor (a conditional branch to `__assert_rtn`). It is taken only
when the precondition described in [Test cases](#test-cases) does not
hold.

| Location | gcov item |
|---|---|
| `keystore.cpp:11` | branch 0 |

The other 11 items do not correspond to a condition written in the
source.

**Unreachable with the current `PagePool`.** Exception edges of calls
to `allocateBlock` and `releaseBlock` (`invoke` instructions). These
functions are not declared `noexcept`, so the compiler keeps an
exception path, but their current implementations only update the free
list and cannot throw.

| Location | gcov item | Call |
|---|---|---|
| `keystore.cpp:18` | branch 1 | `releaseBlock` |
| `keystore.cpp:54` | branch 1 | `allocateBlock` |
| `keystore.cpp:57` | branch 1 | `releaseBlock` |
| `keystore.cpp:108` | branch 1 | `allocateBlock` |

**Unreachable.** Exception edges of calls that cannot throw with these
argument types.

| Location | gcov item | Call |
|---|---|---|
| `keystore.cpp:85` | branch 1 | `std::min` on `std::size_t` values |
| `keystore.cpp:91` | branch 1 | `std::max` on `Version` values |
| `keystore.cpp:126` | branch 1 | Constructor of `std::optional<KeyRef>` from a `KeyRef`, a struct of a pointer and integer members |

**Not attributed to a single instruction.** gcov reports more
two-way branches on this line than the IR has `invoke` instructions.
The IR for lines 126 to 131 contains no conditional branch on a program
value, only one `invoke` instruction and unconditional branches, so
these items do not correspond to a source condition. In each case the
other outcome is taken on every execution of the line.

| Location | gcov item |
|---|---|
| `keystore.cpp:126` | branches 3, 5, 7 and 9 |

## Limitations

- No allocation-failure injection was performed: a `std::bad_alloc`
  from the `std::vector` of reserved blocks in `appendKey` was not
  exercised. The vector allocation is a plain call
  in the generated code, so gcov reports no branch for it.
- The exception edges from `PagePool` calls are unreachable only while
  `PagePool` keeps its current implementation.
- Four gcov items could not be attributed to a single instruction; see
  [Uncovered generated branches](#uncovered-generated-branches).
- No condition coverage was measured: the toolchain does not provide
  it. The isolation of the conditions of `isValid` and of both
  `compareKeys` overloads rests on the test cases listed above and on the llvm-cov branch
  counts.
- Apart from `compareKeys`, multi-block keys are traversed by the caller
  using the exposed block headers and block size. The current interface
  does not encapsulate fragment traversal for reading.
- The maximum key length (65,535 bytes) was not tested. The largest key
  tested is 65,512 bytes, which fills one 64 KiB block.
- A build with `NDEBUG` defined removes the block-size check; such a
  build was not tested.
- Use of a `KeyRef` after it has become invalid, other than calling
  `isValid`, is a caller error and is not tested; this includes passing
  it to `compareKeys`.
- Concurrent access and the no-reclamation-during-access rule are not
  tested.
- Results apply to the source and environment identified above.

## Evidence

- [Test output](evidence/keystore-tests.txt): output of `make -B test`
  for the full suite (80 tests, PagePool, KeyStore and BNodeStore). It
  is the same run as `pagepool-tests.txt` and `bnodestore-tests.txt`.
- [Coverage summary](evidence/keystore-coverage.txt): gcov output after
  running only `./tests.exe /resolver/KeyStore`. The filter does not
  appear in the gcov output; it is recorded in
  [Reproduction](#reproduction).
- [Annotated coverage](evidence/keystore.cpp.gcov): from the same gcov
  run.
- [llvm-cov summary](evidence/keystore-llvm-report.txt): llvm-cov report
  after running only `build/coverage-llvm/tests.exe /resolver/KeyStore`.
  The filter does not appear in the output.
- [llvm-cov annotated coverage](evidence/keystore-llvm-show.txt):
  `keystore.cpp` with line and branch counts, from the same llvm-cov
  run.
