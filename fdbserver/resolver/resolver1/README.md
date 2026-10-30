# resolver v2 (resolver1)

resolver1 is a temporal conflict B+ tree, `TempConflictBTree`, for the FoundationDB Resolver.
Resolver v2 is this directory as published on this branch, integrated in FoundationDB as
btree-v2. Its code is the development state `dc2802fc2f` plus one fix, the reservation of room
for a write's longest keys (see [Write reservation](#write-reservation)); the published commits
have their own hashes.

| Version | Read check | Commit |
|---|---|---|
| Resolver v1 (btree-v1) | One read at a time | `aff127286e68ea7933dc9ef502a7dbd760e1e43d`, tag `resolver-v1` |
| Resolver v2 (btree-v2) | A whole batch, grouped and prefetched | This branch |

## What it implements

- **History:** disjoint key intervals, each with the commit version (CV) of the last write that
  covered it, ordered by key in a B+ tree.
- **Reads:** a read range at read version RV conflicts when it meets an interval with a CV above
  RV. `checkReads<G>` checks a whole conflict batch, laid out as FoundationDB's `ConflictBatch`
  lays it out: reads ordered by begin key are partitioned recursively among the children of each
  node, so nearby reads share a descent, and up to G tasks interleave their steps, each
  prefetching what its next step reads. A read below the boundary marks its transaction too old.
- **Writes:** each accepted write range overwrites the intervals it covers and keeps the parts
  outside it (`addWrites`).
- **Memory by capacity:** nodes (`BNodeStore`) and keys (`KeyStore`) take fixed-size blocks from
  one pool (`PagePool`). When a write needs room, the oldest history is reclaimed and the
  boundary `reclaimCV` advances; a read older than the boundary is rejected as too old instead of
  being checked against history that is gone.
- **Blocks:** 2 KiB by default. `-DRESOLVER1_BLOCK_SIZE=<bytes>` (`make BLOCK_SIZE=4096`)
  chooses another size when compiling; the FoundationDB integration uses 2 KiB.

## Write reservation

Before storing a write, `addWrites` reclaims history until the pool can hold the write's range
and two exteriors, each two keys as long as the longest key stored so far or in the write, plus
two nodes, and never fewer than 35 blocks. Before this fix the reserve was always 35 blocks,
which with 2 KiB blocks covers keys of up to 11,132 bytes. FDB's client sends write conflict
ranges with system keys of up to 30,001 bytes, which need up to 92 blocks: once the pool was
full, such a write could fail to store a key, and the failed store was dereferenced. If the pool
cannot hold one write even after reclaiming everything, `addWrites` now throws
`std::length_error`.

The fix is its own commit on this branch. `AddWritesReclaimsRoomForLongKeys` crashes without it.
The reserve does not decrease after a long key is reclaimed; its effect on `reclaimCV` is
measured in `tests/TempConflictBTreeTests.md`.

## Measurements and the code they measured

The published measurements of resolver v2 were taken before the write-reservation fix, on the
development state `dc2802fc2f`, with binaries built from their own commits. Those commits and
binary hashes identify the measurements; they are not the commits of this branch. On the
benchmark workloads the fixed code makes the same decisions (the same decision hashes, with and
without reclamation), and keys there never exceed the old reserve, but those results are not
measurements of the fixed code.

## Standalone build and tests

resolver1 builds and tests on its own, without FoundationDB:

```sh
cd fdbserver/resolver/resolver1
make test                  # C++20 compiler
make -B test BLOCK_SIZE=4096  # the suite at another block size
bash tests/coverage.sh     # gcov and llvm-cov coverage of every component
```

The suite has 228 tests (15 PagePool, 41 KeyStore, 47 BNodeStore and 125 TempConflictBTree) and
passes at 1, 2, 4 and 8 KiB blocks. Among them, `CheckReadsPastSeparatorsInReusedKeyBlock` forces
the reclamation and reuse of the key block that expired separators point into, and fails if the
refresh of expired separators in `checkReads` is reverted. The validation reports of the
components are in `tests/*Tests.md`. `tests/coverage.sh`
regenerates the annotated coverage reports in `tests/evidence/`, which are not tracked.

`bench/blocksizebench.cpp` measures the tree at the block size it is compiled with:

```sh
clang++ -std=c++20 -O3 -DNDEBUG -I. bench/blocksizebench.cpp \
    pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp -o blocksizebench
./blocksizebench <points|ranges> <batches> <budget MiB>
```

## FoundationDB integration

- `fdbserver/resolver/BTreeV2.cpp` compiles resolver1's sources inside namespace `btree_v2`.
  The only change to them inside FDB is the `RESOLVER1_IN_FDB` branch of `fdbcompat.h`, which
  uses FDB's `StringRef` and the batch records of `fdbserver/resolver/ResolverBatchTypes.h`,
  shared with `ConflictSet.cpp`.
- `fdbserver/resolver/ConflictSet.cpp` resolves every batch of the binary with btree-v2: one
  `checkReads` call per batch, the insertion of the combined writes and the end of the batch call
  `TempConflictBTree` directly. The rest of `ConflictBatch` is FoundationDB's.
- The CMake cache variable `BTREE_V2_TASKS` sets the task capacity G of `checkReads`, 16 by
  default (`cmake -DBTREE_V2_TASKS=24 ...`).
- `--knob_paged_resolver_pool_pages=N` sets btree-v2's pool to N pages of 64 KiB (16,384 by
  default, 1 GiB). The pool is allocated whole when the Resolver's conflict set is built.
- The Resolver logs `ResolverConflictHistory` with `Index=btree-v2` and the pool's pages.

FoundationDB's own skip list, the baseline of the comparison, is the binary built at the
harness commit `1dfa7ffe6a5210c66e36a628552d10a7046649d0`.

Every commit of this branch builds in FoundationDB (Release, Clang 19.1.5) and passes FDB's
`/fdbserver/conflictset/` and `/fdbserver/skiplist/` unit tests. At the head, the smoke replay
of the Resolver's batch path, six cases of 20,000 to 30,000 transactions, matches FDB's skip
list with room and the reference replay under memory pressure, with no mismatch. It also
matches, batch by batch, the binary of the measurements, built from `dc2802fc2f` before the
write-reservation fix.

## Evaluation tools

- `fdbserver/resolver/by5eval/` builds the measurement binaries and runs and analyses the
  comparison at equal memory (6 GiB of history).
- `fdbserver/resolver/by5gen/` holds the trace generator and the hashes of the traces used.
- `fdbserver -r by5resolvertest` is the harness that replays a trace through the Resolver's
  batch path.

## Documentation

The design and evaluation documents are published in
<https://github.com/ricardojimenezperis/fdb-beyond-5s>:

- Resolver v2: <https://github.com/ricardojimenezperis/fdb-beyond-5s/tree/main/resolver/v2>
- Resolver v1: <https://github.com/ricardojimenezperis/fdb-beyond-5s/tree/main/resolver/v1>, with
  the article <https://ricardojimenezperis.com/systems/fdb-resolver-v1/> as its readable account

## Author

Ricardo Jimenez-Peris

- Website: <https://ricardojimenezperis.com>
- GitHub: <https://github.com/ricardojimenezperis>
