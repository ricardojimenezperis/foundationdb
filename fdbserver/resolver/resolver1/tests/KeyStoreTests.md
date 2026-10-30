# KeyStore validation

## Scope

Validation of variable-length key storage in blocks taken from a shared
`PagePool`: key append within one block and across linked blocks, the
empty key, reservation of the first block on demand rather than at
construction, all-or-nothing reservation of the blocks a key needs,
including a new open block,
recovery after pool exhaustion, oldest-first reclamation, `maxCV` tracking,
generation-based validity of references, byte-wise lexicographic
comparison of stored keys with each other and of an external contiguous
key (`StringRef`) with a stored key, and storage of key ranges: a point
range `[k, k\0)` stored once, a general range stored as its begin
followed by its end, comparison with either end, new ranges that copy
the begin or the end of a stored range from the store, and the storage
policy for copies of trimmed ranges described below.

Every reference is a `RangeRef`. A key stored with `appendKey` is a
`RangeRef` whose begin is the key and whose end is empty; it is read
through `keyStart`, `keyBlock` and its begin length, compared with
`compareKeys`, and checked with `isKeyValid`, for which a reference with
an empty begin, which has no storage, is always valid. A range stored
with `appendRange` is checked with `isValid`, which depends only on its
generation, and compared with `compareBegin` and `compareEnd`; its begin
can also be compared as a key with `compareKeys`.

Outside the scope of this component: `PagePool` itself (see
[PagePoolTests.md](PagePoolTests.md)), the choice of the commit-version
cutoff for reclamation, which belongs to the caller, the B+ tree that
holds key references, and concurrent access. `fdbcompat.h` provides
stand-ins for FoundationDB types (`StringRef`) so that the component
builds without `flow`, and includes `../ResolverBatchTypes.h`, the batch
records shared with FoundationDB's `ConflictSet.cpp`; neither is
measured as part of this component. The rule that no
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

- Source commit: the tested files are identified by content; this report
  is committed together with them. SHA-256 (first 16 hex digits):

  | File | SHA-256 |
  |---|---|
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
  | `tests/keystoretests.cpp` | `4490adc5be7f9623` |
  | `tests/pagepooltests.cpp` | `059580ab0c6dff71` |

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
for f in pagepool keystore bnodestore tempconflictbtree testmain tests/pagepooltests tests/keystoretests tests/bnodestoretests tests/tempconflictbtreetests; do
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
g++ -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=. pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp testmain.cpp tests/*.cpp -o build/coverage-llvm/tests.exe
LLVM_PROFILE_FILE=build/coverage-llvm/keystore.profraw build/coverage-llvm/tests.exe /resolver/KeyStore
xcrun llvm-profdata merge -o build/coverage-llvm/keystore.profdata build/coverage-llvm/keystore.profraw
xcrun llvm-cov report build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/keystore.profdata '-ignore-filename-regex=(pagepool|bnodestore|tempconflictbtree|fdbcompat|ResolverBatchTypes|test)' > build/coverage-llvm/keystore-llvm-report.txt
xcrun llvm-cov show build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/keystore.profdata -show-branches=count '-ignore-filename-regex=(pagepool|bnodestore|tempconflictbtree|fdbcompat|ResolverBatchTypes|test)' > build/coverage-llvm/keystore-llvm-show.txt
```

Only the KeyStore tests are run. Each collection starts from an empty
directory, so counters from earlier runs are not merged in. The
`-ignore-filename-regex` option removes the `PagePool`, `BNodeStore`
and `TempConflictBTree` sources, `fdbcompat.h`, `ResolverBatchTypes.h`
and the test sources from the llvm-cov output. `keystore.h` and `common.h`
contain no function bodies, so `keystore.cpp` is the only KeyStore file with
instrumented code.

`tests/coverage.sh` runs these commands for the four components, after
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
| `/resolver/KeyStore/AppendRangeEmptyBegin` | With an empty begin, the point `["", "\0")` and the general range `["", "x")` store only their end, with `endStart` 0. Both ranges become invalid for `isValid` when their block is reclaimed, while `isKeyValid` still reports the reference valid, because its begin is empty. | PASS |
| `/resolver/KeyStore/RangeReadsBackItsBegin` | A range read as a key, through `keyBlock`, `keyStart` and its begin length, reads back its begin, which starts after a 9-byte key and continues in the next block. | PASS |
| `/resolver/KeyStore/AppendRangeEndAtBlockBoundary` | With begin one byte shorter than, equal to and one byte longer than the first block's free space, both views read back; when begin fills the block, end starts at the first data byte of the next block. `compareEnd` orders keys differing in the first and last byte of end. | PASS |
| `/resolver/KeyStore/AppendRangeAcrossBlocks` | A record starting mid-block and spanning at least five blocks reads back through both views; every block of the record records the range's CV. | PASS |
| `/resolver/KeyStore/AppendRangeIsAllOrNothing` | A range needing three blocks with one free fails and returns its reserved block to the pool; the open block is untouched and the next range follows the first one in it. | PASS |
| `/resolver/KeyStore/CompareRangeEndsAgainstReference` | 60 random ranges, points and general ranges, some spanning blocks, compared with 180 keys (random keys and every begin and end) through `compareBegin`, `compareEnd` and `compareKeys` with the range as a key, against the lexicographic order of `std::vector<std::byte>`. | PASS |
| `/resolver/KeyStore/CopiesWithOldCVKeepOldBlocksIntact` | `[a, z)` with CV 10 fills and closes its block. Storing `[d, m)` with CV 20, the copies `[a, d)` and `[m, z)` with CV 10 and a point with CV 5 leaves the old block byte for byte unchanged with `maxCV` 10; every copy is in a later block with a newer generation; each new block records the maximum CV of the records it holds; reclamation takes the old block first, with CV 10, invalidating only `[a, z)`, and then each new block with its maximum CV, invalidating the records that start in it. | PASS |
| `/resolver/KeyStore/AppendRangeKeepsSourceBegin` | `appendRange(source, end, cv)` stores `[source.begin, end)`. From a source whose begin spans three blocks, with an end one byte longer than that begin, the new range has a newer generation and reads back, and the source is unchanged. From the point source `[abc, abc\0)`, the end `abc\0` gives a point stored once, and `abc\x01`, `abd\0` and `z` give general ranges. From a source with an empty begin, the end `\0` gives a point and `x` a general range. | PASS |
| `/resolver/KeyStore/AppendRangeKeepsSourceEnd` | `appendRange(begin, source, cv)` stores `[begin, source.end)`. From a source whose begin spans two blocks and whose end spans blocks after it, the new range reads back and the source is unchanged. From the point source `[abc, abc\0)` with begin `abc`, the result is the same point, copied from the store. From sources with an empty begin, `[abc, xyz)`, `[abc, abc\x01)`, `[abc, abd\0)` and `["", x)` give general ranges and `["", \0)` a point. | PASS |
| `/resolver/KeyStore/AppendRangeSourceEndInNextBlock` | The source's begin fills its first block, so its end starts in the next block (`endStart` 40). A new begin equal to that end without its final zero gives a point, detected by reading the zero in the next block, with `endStart` 0. | PASS |
| `/resolver/KeyStore/AppendRangeTrimsSource` | Trimming `[a, z)` with CV 10 by `[d, m)` with CV 20 stores `[a, d)` from the source's begin and `[m, z)` from its end, with CV 10, and `[d, m)` from external keys; all three read back and the source is unchanged. | PASS |
| `/resolver/KeyStore/AppendRangeFromSourceIsAllOrNothing` | With one free block, a range from the source's begin and a range from its end, each needing the open block and two more, fail and return their reserved block to the pool; the source still reads back. | PASS |
| `/resolver/KeyStore/AppendRangeFromSourceAgainstReference` | 120 new ranges, each from a newly stored random source (points and general ranges, some spanning blocks), alternately keeping the source's begin and its end with a random key, 60 of each; each new range reads back through both views, is stored once when it is a point and as begin followed by end otherwise, and the source still reads back. | PASS |

Every comparison test checks the sign of the result, not its value, and
checks both argument orders. Each pair of keys is compared twice: the two
stored keys with each other, and the contiguous bytes of each key, as a
`StringRef`, with the other stored key. `CompareKeysAgainstReference`
also compares every external key with every stored key, 6,400 more
comparisons. The counts of keys with a replaced byte in
`CompareKeysAgainstReference`, and the number of points, one, among
the new ranges of `AppendRangeFromSourceAgainstReference`, come from
replaying their random decisions with the same seeds, not from the test
output.

In the tests that build new ranges from a stored source, a new range is
checked through both views, and its `endStart` is 0 when it is a point
and the length of its begin otherwise.

41 KeyStore tests passed, 0 failed. The full run, including the 15
PagePool tests, the 47 BNodeStore tests and the 124 TempConflictBTree
tests, reports 228 passed and 0 failed.

These preconditions are checked with `assert`, which terminates the
process when it fails and is removed when `NDEBUG` is defined. The
tested builds do not define `NDEBUG`. Inputs that do not meet them are
outside the interface and are not tested.

- The constructor requires a pool block size larger than
  `sizeof(KeyBlockHeader)`.
- `appendRange` requires the keys it receives as `StringRef` to be no
  longer than 65,535 bytes.
- `appendRange` from a stored source requires the source to be valid
  (`isValid`).
- `compareBegin` and `compareEnd` require a valid range (`isValid`).
- `compareKeys` requires valid keys (`isKeyValid`).

`appendRange` also requires `begin < end`, so that the range is not
empty and always has stored bytes; for the overloads that take a stored
source, the range formed with the source's begin or end must not be
empty. This precondition is documented in `keystore.h` but not checked,
because checking it would compare the bytes of both keys on every call.
The tests check it before calling `appendRange`, in their
`appendRange`, `appendKeepingBegin` and `appendKeepingEnd` helpers.

The helper that reads keys back asserts that each block in the chain is
non-null, so a broken chain fails the test instead of reading invalid
memory.

## Coverage

Coverage was collected for `keystore.cpp`, running only the KeyStore
tests.

gcov:

| Metric | Result |
|---|---|
| Lines executed | 100.00% of 310 |
| Branches executed | 100.00% of 160 |
| Taken at least once | 84.38% of 160 |

llvm-cov source-based coverage:

| Metric | Result |
|---|---|
| Regions | 100.00% of 179 |
| Functions | 100.00% of 26 |
| Lines | 100.00% of 355 |
| Branches | 100.00% of 120 |

These are gcov and llvm-cov metrics. They are not a measurement of
MC/DC, and full coverage does not show that the code is free of
errors. "Taken at least once" does not mean that every decision was
taken in both directions. Apple clang 15 provides no condition
coverage equivalent to GCC's `-fcondition-coverage`.

gcov counts branches between the basic blocks of the generated code,
including the exception edges the compiler adds and the failure paths
of the `assert` checks. llvm-cov counts the true and false outcomes of
each condition written in the source outside the `assert` checks, whose
conditions come from a macro in a system header: its 120 branches are
the 60 conditions of `keystore.cpp`, each taken in both directions.

### Independent condition checks

There are ten compound decisions. llvm-cov reports every one of their
conditions taken in both directions.

`isKeyValid(RangeRef)`: `ref.beginLength == 0 || ref.blockGen >
lastReclaimedKeyGeneration_` (lines 221-222).

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| Empty key | `beginLength == 0` | Not evaluated | Valid | `EmptyKeyWithoutFreeBlocks` (after reclaiming every block) |
| Block not yet reclaimed | `beginLength > 0`, generation above the last reclaimed | False | Valid | `IsValid` |
| Block reclaimed | `beginLength > 0`, generation at or below the last reclaimed | False | Invalid | `IsValid` |

Comparison of two stored keys, `compareKeys(const RangeRef&, const
RangeRef&)`. The loop runs while both keys have bytes left, `rem1 != 0 &&
rem2 != 0` (line 245), and each key moves to its next block when its
fragment is used up and bytes remain, `bytes1 == 0 && rem1 != 0`
(line 257) and the same for the second key (line 262).

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| First key exhausted | `rem1 == 0` | Not evaluated | Stop | `CompareKeysPrefix`, shorter key first |
| Second key exhausted | `rem2 == 0` | `rem1 != 0` | Stop | `CompareKeysPrefix`, shorter key second |
| Both keys have bytes left | `rem1 != 0`, `rem2 != 0` | True | Continue | Every comparison of non-empty keys |
| Fragment not used up | `bytes1 > 0` | Not evaluated | Stay | Every comparison |
| Key ended with its fragment | `bytes1 == 0`, `rem1 == 0` | True | Stay | Equal keys, and a key that is a prefix of the other |
| Key continues in the next block | `bytes1 == 0`, `rem1 != 0` | True | Move | `CompareKeysMultiBlock`, `CompareKeysAgainstReference` |

Comparison of an external key with stored bytes, `compareFrom`, used by
`compareKeys(const StringRef&, const RangeRef&)`, `compareBegin`,
`compareEnd` and the point detection of the overloads that take a stored
source. Only the stored side moves between blocks. The loop condition
`rem1 != 0 && rem2 != 0` (line 297) and the move condition
`bytes2 == 0 && rem2 != 0` (line 308) have the cases of the tables
above. After the loop, the keys are equal when both are exhausted,
`rem1 == 0 && rem2 == 0` (line 314):

| Condition | Input | Other condition | Decision | Test |
|---|---|---|---|---|
| External key not exhausted | `rem1 != 0` | Not evaluated | Not equal | A stored key that is a proper prefix of the external key |
| Stored key not exhausted | `rem1 == 0`, `rem2 != 0` | True | Not equal | An external key that is a proper prefix of the stored key |
| Both exhausted | `rem1 == 0`, `rem2 == 0` | True | Equal | `CompareKeysEqual`, `CompareKeysMultiBlock` |

Detection of a point range. Each `appendRange` overload decides whether
`[begin, end)` is a point with the same three conditions: `endLength ==
beginLength + 1 && the last byte of end is 0 && (beginLength == 0 ||
begin is a prefix of end)`. With two external keys (lines 335-338), the
prefix check is a `memcmp`. With the begin of a stored source,
`appendWithStoredBegin` (lines 368-372), it compares the bytes of end
with the stored begin through `compareFrom`. With the end of a stored
source, `appendWithStoredEnd` checks the length first (line 390) and
then, reading end from the store, its last byte and the prefix through
`compareFrom` (lines 394-396).

Two external keys:

| Condition | Input | Other conditions | Decision | Test |
|---|---|---|---|---|
| End not one byte longer | `[abc, xyz)` | Not evaluated | General | `AppendRangeGeneralStoresBeginThenEnd` |
| Last byte of end not zero | `[abc, abc\x01)` | Length true | General | `AppendRangeGeneralStoresBeginThenEnd` |
| Empty begin | `["", "\0")` | Length and zero true | Point | `AppendRangeEmptyBegin` |
| Begin is a prefix of end | `[abc, abc\0)` | Length and zero true, begin not empty | Point | `AppendRangePointStoresKeyOnce` |
| Begin is not a prefix of end | `[abc, abd\0)` | Length and zero true, begin not empty | General | `AppendRangeGeneralStoresBeginThenEnd` |

Begin of a stored source, all in `AppendRangeKeepsSourceBegin`:

| Condition | Input | Other conditions | Decision | Source |
|---|---|---|---|---|
| End not one byte longer | `[abc, z)` | Not evaluated | General | `[abc, abc\0)` |
| Last byte of end not zero | `[abc, abc\x01)` | Length true | General | `[abc, abc\0)` |
| Empty begin | `["", "\0")` | Length and zero true | Point | `["", q)` |
| Begin is a prefix of end | `[abc, abc\0)` | Length and zero true, begin not empty | Point | `[abc, abc\0)` |
| Begin is not a prefix of end | `[abc, abd\0)` | Length and zero true, begin not empty | General | `[abc, abc\0)` |

End of a stored source, all in `AppendRangeKeepsSourceEnd` unless
stated:

| Condition | Input | Other conditions | Decision | Source |
|---|---|---|---|---|
| End not one byte longer | `[abc, xyz)` | Not evaluated | General | `["", xyz)` |
| Last byte of end not zero | `[abc, abc\x01)` | Length true | General | `["", abc\x01)` |
| Empty begin | `["", "\0")` | Length and zero true | Point | `["", "\0")` |
| Begin is a prefix of end | `[abc, abc\0)` | Length and zero true, begin not empty | Point | `[abc, abc\0)`; also `AppendRangeSourceEndInNextBlock`, with the zero in the block after the one where end starts |
| Begin is not a prefix of end | `[abc, abd\0)` | Length and zero true, begin not empty | General | `["", abd\0)` |

llvm-cov counts for the compound decisions:

| Location | Condition | True | False |
|---|---|---|---|
| `keystore.cpp:221` | `beginLength == 0` | 194 | 30.0k |
| `keystore.cpp:222` | `blockGen > lastReclaimedKeyGeneration_` | 30.0k | 6 |
| `keystore.cpp:245` | `rem1 != 0` | 18.9k | 1.07k |
| `keystore.cpp:245` | `rem2 != 0` | 17.9k | 981 |
| `keystore.cpp:257` | `bytes1 == 0` | 7.00k | 6.57k |
| `keystore.cpp:257` | `rem1 != 0` | 5.93k | 1.07k |
| `keystore.cpp:262` | `bytes2 == 0` | 7.00k | 6.57k |
| `keystore.cpp:262` | `rem2 != 0` | 5.93k | 1.07k |
| `keystore.cpp:297` | `rem1 != 0` | 74.6k | 6.88k |
| `keystore.cpp:297` | `rem2 != 0` | 68.4k | 6.28k |
| `keystore.cpp:308` | `bytes2 == 0` | 36.4k | 5.52k |
| `keystore.cpp:308` | `rem2 != 0` | 29.5k | 6.90k |
| `keystore.cpp:314` | `rem1 == 0` | 6.88k | 6.28k |
| `keystore.cpp:314` | `rem2 == 0` | 986 | 5.89k |
| `keystore.cpp:335` | `endLength == beginLength + 1` | 189 | 112 |
| `keystore.cpp:336` | `end[beginLength] == 0` | 182 | 7 |
| `keystore.cpp:337` | `beginLength == 0` | 3 | 179 |
| `keystore.cpp:338` | Begin is a prefix of end | 177 | 2 |
| `keystore.cpp:368` | `endLength == beginLength + 1` | 8 | 61 |
| `keystore.cpp:369` | `end[beginLength] == 0` | 3 | 5 |
| `keystore.cpp:370` | `beginLength == 0` | 1 | 2 |
| `keystore.cpp:371` | Stored begin is a prefix of end | 1 | 1 |
| `keystore.cpp:390` | `endLength == beginLength + 1` | 8 | 62 |
| `keystore.cpp:394` | Stored last byte of end is 0 | 5 | 3 |
| `keystore.cpp:395` | `beginLength == 0` | 1 | 4 |
| `keystore.cpp:396` | Begin is a prefix of the stored end | 3 | 1 |

`isKeyValid` is evaluated mostly by the `assert` checks of
`compareKeys`, which is why its counts are large; the cases of its table
are the calls made directly by the tests.

The other decisions in `keystore.cpp` have a single condition, and
llvm-cov reports both outcomes covered for each of them. They include
the copy of a record from two sources in `appendRecord` (line 123: the
second source starts when the first is used up), the move of a stored
source to its next block while it is copied (line 135), and the loop in
`locate` (line 458) that skips whole blocks to reach a position in a
record, used by `compareBegin`, `compareEnd` and `appendWithStoredEnd`,
including a position exactly at the first data byte of a later block.

The tests' block arithmetic assumes `sizeof(KeyBlockHeader) == 24`,
which holds on the tested platform: keys appended to a fresh block
start at offset 24. `sizeof(RangeRef) == 24` is checked at compile time.

### Uncovered generated branches

gcov reports 25 untaken branches. Each was compared with the LLVM IR
that Apple clang 15 generates for `keystore.cpp` at `-O0`, with and
without `--coverage`. Line numbers refer to `keystore.cpp`.

**Precondition violations.** The paths to `__assert_rtn` taken when an
`assert` fails, which happens only when a precondition listed in
[Test cases](#test-cases) does not hold. The IR contains a call to
`__assert_rtn` on each of these lines.

| Location | gcov item | Check |
|---|---|---|
| `keystore.cpp:11` | branch 0 | Block size larger than the header |
| `keystore.cpp:55` | branch 0 | A record has at least one byte |
| `keystore.cpp:234` | branches 0 and 2 | Both keys valid (`isKeyValid`), one branch per key |
| `keystore.cpp:279` | branch 0 | Stored key valid (`isKeyValid`) |
| `keystore.cpp:328` | branch 1 | Begin no longer than 65,535 bytes |
| `keystore.cpp:329` | branch 0 | End no longer than 65,535 bytes |
| `keystore.cpp:347` | branch 0 | Source valid (`isValid`) |
| `keystore.cpp:355` | branch 0 | Source valid (`isValid`) |
| `keystore.cpp:363` | branch 0 | End no longer than 65,535 bytes |
| `keystore.cpp:381` | branch 0 | Begin no longer than 65,535 bytes |
| `keystore.cpp:433` | branch 0 | Range valid (`isValid`) |
| `keystore.cpp:438` | branch 0 | Range valid (`isValid`) |

The check on line 55 cannot fail through the interface: `appendKey`
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
| `keystore.cpp:82` | branch 1 | `allocateBlock` |
| `keystore.cpp:85` | branch 1 | `releaseBlock` |
| `keystore.cpp:164` | branch 1 | `allocateBlock` |

**Exception paths not exercised in standard library calls.** Exception
edges of calls with integer or trivially copyable arguments. No
operation in them throws for these types, but they are not declared
`noexcept` and no fault injection was performed.

| Location | gcov item | Call |
|---|---|---|
| `keystore.cpp:117` | branch 1 | `std::min` on `std::size_t` values |
| `keystore.cpp:129` | branch 1 | `std::min` on `std::size_t` values |
| `keystore.cpp:139` | branch 1 | `std::min` on `std::size_t` values |
| `keystore.cpp:148` | branch 1 | `std::max` on `Version` values |
| `keystore.cpp:182` | branch 1 | Constructor of `std::optional<RecordStart>` from a `RecordStart`, a struct of a pointer and integer members |

These are all the `invoke` instructions in the IR of `keystore.cpp`.

**Not attributed to a single instruction.** gcov reports more
two-way branches on this line than the IR has `invoke` instructions.
The IR for lines 182 to 186 contains no conditional branch on a program
value, only one `invoke` instruction and unconditional branches, so
these items do not correspond to a source condition. In each case the
other outcome is taken on every execution of the line.

| Location | gcov item |
|---|---|
| `keystore.cpp:182` | branches 3, 5 and 7 |

## Limitations

- No allocation-failure injection was performed: a `std::bad_alloc`
  from the `std::vector` of reserved blocks in `appendRecord` was not
  exercised. The vector allocation is a plain call
  in the generated code, so gcov reports no branch for it.
- No fault injection was performed for the exception edges of the
  `PagePool` calls. Their current implementations contain no throwing
  operation; a change in `PagePool` would make these paths relevant.
- Three gcov items could not be attributed to a single instruction; see
  [Uncovered generated branches](#uncovered-generated-branches).
- No condition coverage was measured: the toolchain does not provide
  it. The isolation of the conditions of `isKeyValid`, of both
  `compareKeys` overloads, of `compareFrom` and of the three point
  detections rests on the test cases listed above and on the llvm-cov
  branch counts.
- Apart from the comparisons and the copies made by `appendRange` from
  a stored source, multi-block keys and records are traversed by the
  caller using the exposed block headers and block size. The current
  interface does not encapsulate fragment traversal for reading.
- The maximum key length (65,535 bytes) was not tested. The largest key
  tested is 65,512 bytes, which fills one 64 KiB block.
- A build with `NDEBUG` defined removes the precondition checks; such a
  build was not tested.
- The tests store copies of trimmed ranges directly with `appendRange`.
  Trimming itself, and the atomicity of a tree update made of several
  `appendRange` calls, belong to the tree and are not tested here.
- Concurrent access and the no-reclamation-during-access rule are not
  tested.
- Results apply to the source and environment identified above.

## Evidence

- [Test output](evidence/keystore-tests.txt): output of `make -B test`
  for the full suite (228 tests, PagePool, KeyStore, BNodeStore and
  TempConflictBTree). It is the same run as `pagepool-tests.txt`,
  `bnodestore-tests.txt` and `tempconflictbtree-tests.txt`.
- [Coverage summary](evidence/keystore-coverage.txt): gcov output after
  running only `./tests.exe /resolver/KeyStore`. The filter does not
  appear in the gcov output; it is recorded in
  [Reproduction](#reproduction).
- Annotated coverage, `evidence/keystore.cpp.gcov`: from the same gcov
  run.
- [llvm-cov summary](evidence/keystore-llvm-report.txt): llvm-cov report
  after running only `build/coverage-llvm/tests.exe /resolver/KeyStore`.
  The filter does not appear in the output.
- llvm-cov annotated coverage, `evidence/keystore-llvm-show.txt`:
  `keystore.cpp` with line and branch counts, from the same llvm-cov
  run.

The two annotated reports are generated by `tests/coverage.sh` and are not
tracked; running it on the source identified above regenerates the ones
this report describes.
