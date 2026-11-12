# resolver v3 (resolver1)

resolver1 is a temporal conflict B+ tree, `TempConflictBTree`, for the FoundationDB Resolver.
Resolver v3 is this directory as published on this branch, integrated in FoundationDB as
btree-v3. The final resolver v3 is v3.8 (`55d5f98974`, tag `resolver-v3`) plus the stale write
rank fix `f866c98d` and two test commits, `18260afe` and `2a68fc9b`, which change no production
file.

| Version | Read and write path | Commit |
|---|---|---|
| Resolver v1 (btree-v1) | One read at a time | `aff127286e68ea7933dc9ef502a7dbd760e1e43d`, tag `resolver-v1` |
| Resolver v2 (btree-v2) | Reads checked a whole batch at a time, grouped and prefetched | `491d3f5a6fd02bea43b7b02502e35469afc2a53a`, branch `resolver-v2` |
| Resolver v3 (btree-v3) | Reads and write begins routed together; write tasks interleaved | This branch |

The evaluation of the final resolver v3, its evidence and the release assets are indexed in
[`evaluation/README.md`](evaluation/README.md).

## What it implements

- **History:** disjoint key intervals, each with the commit version (CV) of the last write that
  covered it, ordered by key in a B+ tree.
- **Reads:** a read range at read version RV conflicts when it meets an interval with a CV above
  RV. `checkReads<G>` checks a whole conflict batch, laid out as FoundationDB's `ConflictBatch`
  lays it out: reads ordered by begin key are partitioned recursively among the children of each
  node, so nearby reads share a descent, and up to G tasks interleave their steps, each
  prefetching what its next step reads. A read below the boundary marks its transaction too old.
- **Writes:** read tasks also record where each accepted write will start. A single-threaded
  scheduler then overlaps the searches and prefetches of the write tasks, while the oldest task
  applies writes in key order (`addWrites`). Each accepted write range overwrites the intervals it
  covers and keeps the parts outside it. The tasks form `RESOLVER1_WRITE_GROUPS` groups of
  `RESOLVER1_WRITE_LG` tasks, 2 groups of 8 by default.
- **Memory by capacity:** nodes (`BNodeStore`) and keys (`KeyStore`) take fixed-size blocks from
  one pool (`PagePool`). When a write needs room, the oldest history is reclaimed and the
  boundary `reclaimCV` advances; a read older than the boundary is rejected as too old instead of
  being checked against history that is gone.
- **Page backing:** `RESOLVER1_PAGE_MODE`, read when the pool is built, selects how its pages are
  allocated: `legacy` (the default, `new[]`), `base` (2 MiB-aligned anonymous memory without
  transparent huge pages), `thp` (the same, with transparent huge pages advised) or `hugetlb`
  (explicit 2 MiB HugeTLB pages). With `hugetlb` the pool throws `std::system_error` if the
  mapping fails, so the huge pages must be reserved first. The published performance
  configuration is `hugetlb`.
- **Blocks:** 2 KiB by default. `-DRESOLVER1_BLOCK_SIZE=<bytes>` (`make BLOCK_SIZE=4096`)
  chooses another size when compiling; the FoundationDB integration uses 2 KiB.

## Write reservation

Before storing a write, `addWrites` reclaims history until the pool can hold the write's range
and two exteriors, each two keys as long as the longest key stored so far or in the write, plus
two nodes, and never fewer than 35 blocks. If the pool cannot hold one write even after
reclaiming everything, `addWrites` throws `std::length_error`. The reserve does not decrease
after a long key is reclaimed; its effect on `reclaimCV` is measured in
`tests/TempConflictBTreeTests.md`.

## Stale write ranks after reclamation

`f866c98d` tracks the leaf generation and mutation epoch associated with each saved write rank,
and restarts the search when reclamation, block reuse or another write invalidates that position.
Node and key-block generations are disjoint, so a recycled pool block cannot validate a stale leaf
reference. Without it, v3.8 loses accepted writes when reclamation frees a prepared leaf: with a
pool of 2 pages the case of `AddWritesKeepsWritesAfterReclamationFreesAPreparedLeaf` loses 60
writes with 2 groups of 8 tasks and aborts with 2 groups of 4. The case is the regression test of
`18260afe`.

## Standalone build and tests

resolver1 builds and tests on its own, without FoundationDB:

```sh
cd fdbserver/resolver/resolver1
make test                  # C++20 compiler
make -B test BLOCK_SIZE=4096  # the suite at another block size
bash tests/coverage.sh     # gcov and llvm-cov coverage of every component
```

The suite has 240 tests (19 PagePool, 41 KeyStore, 48 BNodeStore and 132 TempConflictBTree) and
passes at 1, 2, 4 and 8 KiB blocks. Among them, `CheckReadsPastSeparatorsInReusedKeyBlock` forces
the reclamation and reuse of the key block that expired separators point into, and fails if the
refresh of expired separators in `checkReads` is reverted. Commit `02d354f9` adds 8 of the tests
for MC/DC coverage and changes no production file. The suite passes 240/240 in 48 configurations:
24 on Linux with clang 19.1.5 (AMD Ryzen 7 7700) and 24 on macOS with Apple clang 15 (Apple M3
Pro), covering 2 groups of 8 and of 4 write tasks, the four block sizes, and builds with
assertions, with `-O2 -DNDEBUG` and with sanitizers. The validation reports of the
components are in `tests/*Tests.md`. `tests/coverage.sh` regenerates the annotated coverage
reports in `tests/evidence/`. Selected test outputs and coverage summaries are tracked; raw
`.gcov` and `*-llvm-show.txt` files are excluded by `.gitignore` and distributed with the
external evidence.

[`tests/MCDCCoverage.md`](tests/MCDCCoverage.md) reports the MC/DC coverage measured with clang
20.1.8 on the default build, 2 groups of 8 write tasks: 153 of 210 conditions show an
independence pair (72.86%). It classifies each of the other 57 conditions and keeps assertion
conditions in the denominator. The build with `RESOLVER1_WRITE_LG=0` is measured separately and
is not added to these figures.

`bench/blocksizebench.cpp` measures the tree at the block size it is compiled with:

```sh
clang++ -std=c++20 -O3 -DNDEBUG -I. bench/blocksizebench.cpp \
    pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp -o blocksizebench
./blocksizebench <points|ranges> <batches> <budget MiB>
```

`-DBENCH_API` selects the API it drives (3, resolver v3's, by default) and `-DBENCH_TASKS` the
task capacity G (16 by default).

## FoundationDB integration

- `fdbserver/resolver/BTreeV3.cpp` compiles resolver1's sources inside namespace `btree_v3`.
  The only change to them inside FDB is the `RESOLVER1_IN_FDB` branch of `fdbcompat.h`, which
  uses FDB's `StringRef` and the batch records of `fdbserver/resolver/ResolverBatchTypes.h`,
  shared with `ConflictSet.cpp`.
- `fdbserver/resolver/ConflictSet.cpp` resolves every batch of the binary with btree-v3: one
  `checkReads` call per batch, the insertion of the combined writes and the end of the batch call
  `TempConflictBTree` directly. The rest of `ConflictBatch` is FoundationDB's.
- The CMake cache variable `BTREE_V3_TASKS` sets the task capacity G of `checkReads`, 16 by
  default, from 1 to 255 (`cmake -DBTREE_V3_TASKS=24 ...`).
- `--knob_paged_resolver_pool_pages=N` sets btree-v3's pool to N pages of 64 KiB (16,384 by
  default, 1 GiB). The pool is allocated whole when the Resolver's conflict set is built, with
  the backing that `RESOLVER1_PAGE_MODE` selects in the Resolver's environment.
- The Resolver logs `ResolverConflictHistory` with `Index=btree-v3` and the pool's pages.

FoundationDB's own skip list, the baseline of the comparison, is the binary built at the
harness commit `1dfa7ffe6a5210c66e36a628552d10a7046649d0`.

## Evaluation tools

- `fdbserver/resolver/by5eval/` builds the measurement binaries and runs and analyses the
  comparison at equal memory (6 GiB of history).
- `fdbserver/resolver/by5gen/` holds the trace generator and the hashes of the traces used.
- `fdbserver -r by5resolvertest` is the harness that replays a trace through the Resolver's
  batch path.

## Documentation

The design and evaluation documents are published in
<https://github.com/ricardojimenezperis/fdb-beyond-5s>:

- Resolver v3: <https://github.com/ricardojimenezperis/fdb-beyond-5s/tree/main/resolver/v3>
- Resolver v2: <https://github.com/ricardojimenezperis/fdb-beyond-5s/tree/main/resolver/v2>
- Resolver v1: <https://github.com/ricardojimenezperis/fdb-beyond-5s/tree/main/resolver/v1>, with
  the article <https://ricardojimenezperis.com/systems/fdb-resolver-v1/> as its readable account

## Author

Ricardo Jimenez-Peris

- Website: <https://ricardojimenezperis.com>
- GitHub: <https://github.com/ricardojimenezperis>
