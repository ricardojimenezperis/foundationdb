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
key (`StringRef`) with a stored key, and storage of key ranges
(`RangeRef`): a point range `[k, k\0)` stored once, a general range
stored as its begin followed by its end, comparison with either end,
and the storage policy for copies of trimmed ranges described below.

Outside the scope of this component: `PagePool` itself (see
[PagePoolTests.md](PagePoolTests.md)), the choice of the commit-version
cutoff for reclamation, which belongs to the caller, the B+ tree that
holds key references, and concurrent access. `fdbcompat.h` provides
stand-ins for FoundationDB types (`StringRef`) so that the component
builds without `flow`; it is not measured as part of this component. The rule that no
reclamation may occur while key bytes are being accessed is a caller
contract and is not tested here. Trimming ranges in the tree, and the
atomicity of a whole tree update, belong to the tree.

### Storage policy for ranges

The commit version (CV) of a range records the age of its conflict
information; the generation of its `RangeRef` records when its bytes
were allocated. They are independent. When the tree trims a stored
range, it stores the outer parts again with `appendRange`, keeping
their original CV, so a new record can carry an old CV and
`appendRange` accepts CVs in any order. Existing records and closed
blocks are not modified: records are only appended at the open end of
the store, where the open block may receive new records and raise its
`maxCV`; closed blocks are not extended or reopened, and their `maxCV`
is not raised. Each block records the maximum CV of the records it
holds, and blocks are
reclaimed first in, first out, with validity checked by generation.
The tree must not reclaim blocks while it reads their bytes to build
the copies.

## Source and environment

- Source commit: not under version control; tested files identified by
  SHA-256 (first 16 hex digits):

  | File | SHA-256 |
  |---|---|
  | `keystore.cpp` | `051ebb1d9c0f25c3` |
  | `keystore.h` | `6325656bc8268aa1` |
  | `fdbcompat.h` | `788cc62b6648bf6d` |
  | `common.h` | `0a0b864acb86c330` |
  | `pagepool.cpp` | `7506a3e1f65b263a` |
  | `pagepool.h` | `b6549ed8ffe91ea8` |
  | `test.h` | `a9a025cd287fd82e` |
  | `testmain.cpp` | `f977afc8cc420126` |
  | `makefile` | `246b6c0baf2b6a99` |
  | `tests/keystoretests.cpp` | `9c98fd8aea83da34` |
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
| `/resolver/KeyStore/AppendRangePointStoresKeyOnce` | `[abc, abc\0)` is stored once as the 4 bytes `abc\0`: `beginLength` 3, `endStart` 0, `endLength` 4; both views read back their key; the next record starts 4 bytes later. | PASS |
| `/resolver/KeyStore/AppendRangeGeneralStoresBeginThenEnd` | `[abc, xyz)` is stored as `abcxyz` with `endStart` 3. An end one byte longer than begin is a general range when its last byte is not zero (`abc\x01`) or when it does not start with begin (`abd\0`). | PASS |
| `/resolver/KeyStore/AppendRangeEmptyBegin` | With an empty begin, the point `["", "\0")` and the general range `["", "x")` store only their end, with `endStart` 0. `beginKey` gives an empty key, which stays valid, while both ranges become invalid when their block is reclaimed. | PASS |
| `/resolver/KeyStore/BeginKeyViewsTheRecord` | `beginKey` returns a `KeyRef` with the record's block, offset, begin length and generation, and reads back begin across blocks. | PASS |
| `/resolver/KeyStore/AppendRangeEndAtBlockBoundary` | With begin one byte shorter than, equal to and one byte longer than the first block's free space, both views read back; when begin fills the block, end starts at the first data byte of the next block. `compareEnd` orders keys differing in the first and last byte of end. | PASS |
| `/resolver/KeyStore/AppendRangeAcrossBlocks` | A record starting mid-block and spanning at least five blocks reads back through both views; every block of the record records the range's CV. | PASS |
| `/resolver/KeyStore/AppendRangeIsAllOrNothing` | A range needing three blocks with one free fails and returns its reserved block to the pool; the open block is untouched and the next range follows the first one in it. | PASS |
| `/resolver/KeyStore/CompareRangeEndsAgainstReference` | 60 random ranges, points and general ranges, some spanning blocks, compared with 180 keys (random keys and every begin and end) through `compareBegin`, `compareEnd` and `compareKeys` on `beginKey`, against the lexicographic order of `std::vector<std::byte>`. | PASS |
| `/resolver/KeyStore/CopiesWithOldCVKeepOldBlocksIntact` | `[a, z)` with CV 10 fills and closes its block. Storing `[d, m)` with CV 20, the copies `[a, d)` and `[m, z)` with CV 10 and a point with CV 5 leaves the old block byte for byte unchanged with `maxCV` 10; every copy is in a later block with a newer generation; each new block records the maximum CV of the records it holds; reclamation takes the old block first, with CV 10, invalidating only `[a, z)`, and then each new block with its maximum CV, invalidating the records that start in it. | PASS |

Every comparison test checks the sign of the result, not its value, and
checks both argument orders. Each pair of keys is compared twice: the two
stored keys with each other, and the contiguous bytes of each key, as a
`StringRef`, with the other stored key. `CompareKeysAgainstReference`
also compares every external key with every stored key, 6,400 more
comparisons. The counts of keys with a replaced byte in
`CompareKeysAgainstReference` come from replaying its random decisions
with the same seed, not from the test output.

35 KeyStore tests passed, 0 failed. The full run, including the 13
PagePool tests and the 41 BNodeStore tests, reports 89 passed and 0
failed.

These preconditions are checked with `assert`, which terminates the
process when it fails and is removed when `NDEBUG` is defined. The
tested builds do not define `NDEBUG`. Inputs that do not meet them are
outside the interface and are not tested.

- The constructor requires a pool block size larger than
  `sizeof(KeyBlockHeader)`.
- `appendRange` requires both keys no longer than 65,535 bytes.

`appendRange` also requires `begin < end`, so that the range is not
empty and always has stored bytes. This precondition is documented in
`keystore.h` but not checked, because checking it would compare the
bytes of both keys on every call. The tests check it before calling
`appendRange`, in their `appendRange` and `storeLeafRange` helpers.

The helper that reads keys back asserts that each block in the chain is
non-null, so a broken chain fails the test instead of reading invalid
memory.

## Coverage

Coverage was collected for `keystore.cpp`, running only the KeyStore
tests.

gcov:

| Metric | Result |
|---|---|
| Lines executed | 100.00% of 255 |
| Branches executed | 100.00% of 124 |
| Taken at least once | 87.90% of 124 |

llvm-cov source-based coverage:

| Metric | Result |
|---|---|
| Regions | 100.00% of 147 |
| Functions | 100.00% of 19 |
| Lines | 100.00% of 287 |
| Branches | 100.00% of 100 |

These are gcov and llvm-cov metrics. They are not a measurement of
MC/DC, and full coverage does not show that the code is free of
errors. "Taken at least once" does not mean that every decision was
taken in both directions. Apple clang 15 provides no condition
coverage equivalent to GCC's `-fcondition-coverage`.

gcov counts branches between the basic blocks of the generated code,
including the exception edges the compiler adds and the failure paths
of the `assert` checks. llvm-cov counts the true and false outcomes of
each condition written in the source outside the `assert` checks, whose
conditions come from a macro in a system header: its 100 branches are
the 50 conditions of `keystore.cpp`, each taken in both directions.

### Independent condition checks

There are eight compound decisions. llvm-cov reports every one of their
conditions taken in both directions.

`isValid(KeyRef)`: `ref.keyLength == 0 || ref.blockGen >
lastReclaimedKeyGeneration_` (lines 194-195).

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| Empty key | `keyLength == 0` | Not evaluated | Valid | `EmptyKeyWithoutFreeBlocks` (after reclaiming every block) |
| Block not yet reclaimed | `keyLength > 0`, generation above the last reclaimed | False | Valid | `IsValid` |
| Block reclaimed | `keyLength > 0`, generation at or below the last reclaimed | False | Invalid | `IsValid` |

Comparison of two stored keys, `compareKeys(const KeyRef&, const
KeyRef&)`. The loop runs while both keys have bytes left, `rem1 != 0 &&
rem2 != 0` (line 217), and each key moves to its next block when its
fragment is used up and bytes remain, `bytes1 == 0 && rem1 != 0`
(line 229) and the same for the second key (line 234).

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| First key exhausted | `rem1 == 0` | Not evaluated | Stop | `CompareKeysPrefix`, shorter key first |
| Second key exhausted | `rem2 == 0` | `rem1 != 0` | Stop | `CompareKeysPrefix`, shorter key second |
| Both keys have bytes left | `rem1 != 0`, `rem2 != 0` | True | Continue | Every comparison of non-empty keys |
| Fragment not used up | `bytes1 > 0` | Not evaluated | Stay | Every comparison |
| Key ended with its fragment | `bytes1 == 0`, `rem1 == 0` | True | Stay | Equal keys, and a key that is a prefix of the other |
| Key continues in the next block | `bytes1 == 0`, `rem1 != 0` | True | Move | `CompareKeysMultiBlock`, `CompareKeysAgainstReference` |

Comparison of an external key with stored bytes, `compareFrom`, used by
`compareKeys(const StringRef&, const KeyRef&)`, `compareBegin` and
`compareEnd`. Only the stored side moves between blocks. The loop
condition `rem1 != 0 && rem2 != 0` (line 268) and the move condition
`bytes2 == 0 && rem2 != 0` (line 279) have the cases of the tables
above. After the loop, the keys are equal when both are exhausted,
`rem1 == 0 && rem2 == 0` (line 285):

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| External key not exhausted | `rem1 != 0` | Not evaluated | Not equal | A stored key that is a proper prefix of the external key |
| Stored key not exhausted | `rem1 == 0`, `rem2 != 0` | True | Not equal | An external key that is a proper prefix of the stored key |
| Both exhausted | `rem1 == 0`, `rem2 == 0` | True | Equal | `CompareKeysEqual`, `CompareKeysMultiBlock` |

Detection of a point range in `appendRange`: `endLength == beginLength
+ 1 && end[beginLength] == 0 && (beginLength == 0 || begin is a prefix
of end)` (lines 308-311).

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| End not one byte longer | `[abc, xyz)` | Not evaluated | General | `AppendRangeGeneralStoresBeginThenEnd` |
| Last byte of end not zero | `[abc, abc\x01)` | Length true | General | `AppendRangeGeneralStoresBeginThenEnd` |
| Empty begin | `["", "\0")` | Length and zero true | Point | `AppendRangeEmptyBegin` |
| Begin is a prefix of end | `[abc, abc\0)` | Length and zero true, begin not empty | Point | `AppendRangePointStoresKeyOnce` |
| Begin is not a prefix of end | `[abc, abd\0)` | Length and zero true, begin not empty | General | `AppendRangeGeneralStoresBeginThenEnd` |

llvm-cov counts for the compound decisions:

| Location | Condition | True | False |
|---|---|---|---|
| `keystore.cpp:194` | `keyLength == 0` | 5 | 13 |
| `keystore.cpp:195` | `blockGen > lastReclaimedKeyGeneration_` | 7 | 6 |
| `keystore.cpp:217` | `rem1 != 0` | 18.9k | 1.07k |
| `keystore.cpp:217` | `rem2 != 0` | 17.9k | 981 |
| `keystore.cpp:229` | `bytes1 == 0` | 7.00k | 6.57k |
| `keystore.cpp:229` | `rem1 != 0` | 5.93k | 1.07k |
| `keystore.cpp:234` | `bytes2 == 0` | 7.00k | 6.57k |
| `keystore.cpp:234` | `rem2 != 0` | 5.93k | 1.07k |
| `keystore.cpp:268` | `rem1 != 0` | 73.4k | 6.35k |
| `keystore.cpp:268` | `rem2 != 0` | 67.1k | 6.28k |
| `keystore.cpp:279` | `bytes2 == 0` | 35.2k | 5.52k |
| `keystore.cpp:279` | `rem2 != 0` | 28.8k | 6.37k |
| `keystore.cpp:285` | `rem1 == 0` | 6.35k | 6.28k |
| `keystore.cpp:285` | `rem2 == 0` | 458 | 5.89k |
| `keystore.cpp:308` | `endLength == beginLength + 1` | 48 | 31 |
| `keystore.cpp:309` | `end[beginLength] == 0` | 44 | 4 |
| `keystore.cpp:310` | `beginLength == 0` | 2 | 42 |
| `keystore.cpp:311` | Begin is a prefix of end | 41 | 1 |

The other decisions in `keystore.cpp` have a single condition, and
llvm-cov reports both outcomes covered for each of them. They include
the copy of a record from two sources in `appendRecord` (lines 109-110:
the second source starts when the first is used up) and the skip of the
start of a record in `compareAt` (line 361: the start of end is in a
later block, including exactly at its first data byte).

The tests' block arithmetic assumes `sizeof(KeyBlockHeader) == 24`,
which holds on the tested platform: keys appended to a fresh block
start at offset 24. `sizeof(RangeRef) == 24` is checked at compile time.

### Uncovered generated branches

gcov reports 15 untaken branches. Each was compared with the LLVM IR
that Apple clang 15 generates for `keystore.cpp` at `-O0`, with and
without `--coverage`. Line numbers refer to `keystore.cpp`.

**Precondition violations.** The paths to `__assert_rtn` taken when an
`assert` fails, which happens only when a precondition listed in
[Test cases](#test-cases) does not hold. The IR contains a call to
`__assert_rtn` on each of these lines.

| Location | gcov item | Check |
|---|---|---|
| `keystore.cpp:11` | branch 0 | Block size larger than the header |
| `keystore.cpp:44` | branch 0 | A record has at least one byte |
| `keystore.cpp:299` | branch 1 | Begin no longer than 65,535 bytes |
| `keystore.cpp:300` | branch 0 | End no longer than 65,535 bytes |

The check on line 44 cannot fail through the interface: `appendKey`
returns before it for the empty key, and a range always has stored
bytes.

**Exception paths not exercised.** Exception edges of calls to
`allocateBlock` and `releaseBlock` (`invoke` instructions). These
functions are not declared `noexcept`, so the compiler keeps an
exception path. Their current implementations contain no operation that
throws, but no fault injection was performed, so these paths were not
exercised.

| Location | gcov item | Call |
|---|---|---|
| `keystore.cpp:18` | branch 1 | `releaseBlock` |
| `keystore.cpp:71` | branch 1 | `allocateBlock` |
| `keystore.cpp:74` | branch 1 | `releaseBlock` |
| `keystore.cpp:137` | branch 1 | `allocateBlock` |

**Exception paths not exercised in standard library calls.** Exception
edges of calls with integer or trivially copyable arguments. No
operation in them throws for these types, but they are not declared
`noexcept` and no fault injection was performed.

| Location | gcov item | Call |
|---|---|---|
| `keystore.cpp:103` | branch 1 | `std::min` on `std::size_t` values |
| `keystore.cpp:115` | branch 1 | `std::min` on `std::size_t` values |
| `keystore.cpp:121` | branch 1 | `std::max` on `Version` values |
| `keystore.cpp:155` | branch 1 | Constructor of `std::optional<RecordStart>` from a `RecordStart`, a struct of a pointer and integer members |

**Not attributed to a single instruction.** gcov reports more
two-way branches on this line than the IR has `invoke` instructions.
The IR for lines 155 to 159 contains no conditional branch on a program
value, only one `invoke` instruction and unconditional branches, so
these items do not correspond to a source condition. In each case the
other outcome is taken on every execution of the line.

| Location | gcov item |
|---|---|
| `keystore.cpp:155` | branches 3, 5 and 7 |

## Limitations

- No allocation-failure injection was performed: a `std::bad_alloc`
  from the `std::vector` of reserved blocks in `appendKey` was not
  exercised. The vector allocation is a plain call
  in the generated code, so gcov reports no branch for it.
- No fault injection was performed for the exception edges of the
  `PagePool` calls. Their current implementations contain no throwing
  operation; a change in `PagePool` would make these paths relevant.
- Three gcov items could not be attributed to a single instruction; see
  [Uncovered generated branches](#uncovered-generated-branches).
- No condition coverage was measured: the toolchain does not provide
  it. The isolation of the conditions of `isValid` and of both
  `compareKeys` overloads rests on the test cases listed above and on the llvm-cov branch
  counts.
- Apart from the comparisons, multi-block keys and records are
  traversed by the caller using the exposed block headers and block
  size. The current interface does not encapsulate fragment traversal
  for reading.
- The maximum key length (65,535 bytes) was not tested. The largest key
  tested is 65,512 bytes, which fills one 64 KiB block.
- A build with `NDEBUG` defined removes the precondition checks; such a
  build was not tested.
- The tests store copies of trimmed ranges directly with `appendRange`.
  Trimming itself, and the atomicity of a tree update made of several
  `appendRange` calls, belong to the tree and are not tested here.
- Use of a `KeyRef` after it has become invalid, other than calling
  `isValid`, is a caller error and is not tested; this includes passing
  it to `compareKeys`.
- Concurrent access and the no-reclamation-during-access rule are not
  tested.
- Results apply to the source and environment identified above.

## Evidence

- [Test output](evidence/keystore-tests.txt): output of `make -B test`
  for the full suite (89 tests, PagePool, KeyStore and BNodeStore). It
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
