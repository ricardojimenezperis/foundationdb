# resolver v3 (resolver1)

resolver1 is a temporal conflict B+ tree, `TempConflictBTree`, for the FoundationDB Resolver.
Resolver v3 is this directory as published on this branch, integrated in FoundationDB as
btree-v3. The selected implementation is v3.8, tag `resolver-v3`, commit
`55d5f98974b352f3a124439ae86beb0f8b7ca76d`; its tree is the tree of the measured development
commit `d6bba393b9`. This README update is documentation-only; the tag remains at the measured
v3.8 tree.

| Version | Read check | Write application | Commit |
|---|---|---|---|
| Resolver v1 (btree-v1) | One read at a time | One write at a time | `aff127286e68ea7933dc9ef502a7dbd760e1e43d`, tag `resolver-v1` |
| Resolver v2 (btree-v2) | A whole batch, grouped and prefetched | One write at a time | branch `resolver-v2` |
| Resolver v3 (btree-v3) | Reads and write begins routed together | Prepared destinations, interleaved write tasks | tag `resolver-v3`, this branch |

## What it implements

- **History:** disjoint key intervals, each with the commit version (CV) of the last write that
  covered it, ordered by key in a B+ tree.
- **Reads and write begins:** a read range at read version RV conflicts when it meets an interval
  with a CV above RV. `checkReads<G>` takes a whole conflict batch, laid out as FoundationDB's
  `ConflictBatch` lays it out, with the begins of its write ranges among the sorted endpoints.
  Reads and write begins are partitioned recursively among the children of each node, so nearby
  endpoints share a descent, and up to G tasks interleave their steps, each prefetching what its
  next step reads. Read tasks check conflicts and record the leaves where the batch's writes will
  start. A read below the boundary marks its transaction too old.
- **Writes:** `addWrites` applies the accepted write ranges of the batch from the destinations
  `checkReads` prepared. Each accepted write range overwrites the intervals it covers and keeps the
  parts outside it. A single-threaded scheduler overlaps the searches and prefetches of the write
  tasks, while the oldest task applies writes in key order. "Interleaved" means memory latency
  overlapped within one core, not simultaneous mutation of the tree.
- **Write tasks:** by default two alternating groups of eight tasks. `-DRESOLVER1_WRITE_LG=<n>`
  sets the tasks per group (`0` applies writes one at a time) and `-DRESOLVER1_WRITE_GROUPS=3`
  selects a three-group policy. Two groups of eight are the selected configuration.
- **Memory by capacity:** nodes (`BNodeStore`) and keys (`KeyStore`) take fixed-size blocks from
  one pool (`PagePool`). When a write needs room, the oldest history is reclaimed and the
  boundary `reclaimCV` advances; a read older than the boundary is rejected as too old instead of
  being checked against history that is gone.
- **Pool backing:** the environment variable `RESOLVER1_PAGE_MODE` selects `legacy` (the default),
  `base`, `thp` or `hugetlb` (2 MiB pages). An explicit mode fails if its allocation or advice
  fails; there is no fallback.
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

The fix entered resolver v2 as its own commit, and this branch keeps it.
`AddWritesReclaimsRoomForLongKeys` crashes without it. The reserve does not decrease after a long
key is reclaimed; its effect on `reclaimCV` is measured in `tests/TempConflictBTreeTests.md`.

## Measurements and the code they measured

The published measurements of resolver v3 were taken on the development commit `d6bba393b9`,
whose tree is the tree of the tagged commit, with the pool on HugeTLB pages and the whole binary
built with `-falign-functions=64`. They are measurements of this code. The measurements of
resolver v2 were taken before the write-reservation fix, on its development state `dc2802fc2f`;
they are not measurements of this code.

## Standalone build and tests

resolver1 builds and tests on its own, without FoundationDB:

```sh
cd fdbserver/resolver/resolver1
make test                  # C++20 compiler
make -B test BLOCK_SIZE=4096  # the suite at another block size
bash tests/coverage.sh     # gcov and llvm-cov coverage of every component
```

The suite has 228 tests (15 PagePool, 41 KeyStore, 47 BNodeStore and 125 TempConflictBTree) and
passes at 1, 2, 4 and 8 KiB blocks with Clang 19.1.5 and libc++. Among them,
`CheckReadsPastSeparatorsInReusedKeyBlock` forces the reclamation and reuse of the key block that
expired separators point into, and fails if the refresh of expired separators in `checkReads` is
reverted. The validation reports of the components are in `tests/*Tests.md`. `tests/coverage.sh`
regenerates the annotated coverage reports in `tests/evidence/`, which are not tracked.

`bench/blocksizebench.cpp` measures the tree at the block size it is compiled with. The same
source builds against the resolver1 of v1, v2 or v3 through the interface each has
(`-DBENCH_API=1`, `2` or `3`, the default), and times each batch's preparation, read check and
write application:

```sh
clang++ -std=c++20 -O3 -DNDEBUG -I. bench/blocksizebench.cpp \
    pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp -o blocksizebench
./blocksizebench <points|ranges> <batches> <budget MiB>
```

## FoundationDB integration

- `fdbserver/resolver/BTreeV3.cpp` compiles resolver1's sources inside namespace `btree_v3`.
  The only change to them inside FDB is the `RESOLVER1_IN_FDB` branch of `fdbcompat.h`, which
  uses FDB's `StringRef` and the batch records of `fdbserver/resolver/ResolverBatchTypes.h`,
  shared with `ConflictSet.cpp`.
- `fdbserver/resolver/ConflictSet.cpp` resolves every batch of the binary with btree-v3. After
  the endpoints are sorted, each is given its position in its transaction's slot. One
  `checkReads` call per batch, made also for a batch without reads, checks the reads and prepares
  the destinations of the writes; the accepted write ranges are then applied as they are, without
  combining them first. The pending cleanup of expired entries is drained when `checkReads`
  finishes, and `addWrites` reclaims memory when a write needs room. The rest of `ConflictBatch`
  is FoundationDB's.
- The CMake cache variable `BTREE_V3_TASKS` sets the task capacity G of `checkReads`, 16 by
  default (`cmake -DBTREE_V3_TASKS=24 ...`).
- `--knob_paged_resolver_pool_pages=N` sets btree-v3's pool to N pages of 64 KiB (16,384 by
  default, 1 GiB). The pool is allocated whole when the Resolver's conflict set is built.
- The Resolver logs `ResolverConflictHistory` with `Index=btree-v3` and the pool's pages.

FoundationDB's own skip list, the baseline of the comparison, is the binary built at the
harness commit `1dfa7ffe6a5210c66e36a628552d10a7046649d0`.

The tagged commit builds in FoundationDB (Release, Clang 19.1.5) and passes FDB's
`/fdbserver/conflictset/` and `/fdbserver/skiplist/` unit tests. Its smoke replay of the
Resolver's batch path, six cases at room and six under memory pressure, matches FDB's skip list
with room and the reference replay under pressure, with no mismatch. It also matches, batch by
batch, the binary of the measurements.

## Evaluation tools

- `fdbserver/resolver/by5eval/` builds the measurement binaries and runs and analyses the
  comparison at equal memory (6 GiB of history).
- `fdbserver/resolver/by5gen/` holds the trace generator and the hashes of the traces used.
- `fdbserver -r by5resolvertest` is the harness that replays a trace through the Resolver's
  batch path.
- `noSim/fdbserver/conflictset/benchmark` and `noSim/fdbserver/conflictset/macBenchmark`, run
  through `fdbserver -r unittests`, time the batch path and each of its phases.

## Documentation

The design and evaluation documents are published in
<https://github.com/ricardojimenezperis/fdb-beyond-5s>:

- Resolver v3: <https://github.com/ricardojimenezperis/fdb-beyond-5s/tree/main/resolver/v3>, with
  its evaluation <https://github.com/ricardojimenezperis/fdb-beyond-5s/blob/main/resolver/v3/06-evaluation.md>
- Resolver v2: <https://github.com/ricardojimenezperis/fdb-beyond-5s/tree/main/resolver/v2>
- Resolver v1: <https://github.com/ricardojimenezperis/fdb-beyond-5s/tree/main/resolver/v1>, with
  the article <https://ricardojimenezperis.com/systems/fdb-resolver-v1/> as its readable account

## Author

Ricardo Jimenez-Peris

- Website: <https://ricardojimenezperis.com>
- GitHub: <https://github.com/ricardojimenezperis>
