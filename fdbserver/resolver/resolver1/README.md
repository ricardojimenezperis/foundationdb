# resolver v1 (resolver1)

resolver1 is a temporal conflict B+ tree, `TempConflictBTree`, for the FoundationDB Resolver.
Resolver v1 is this directory at commit `aff127286e68ea7933dc9ef502a7dbd760e1e43d`, integrated
in FoundationDB as btree-v1.

## What it implements

- **History:** disjoint key intervals, each with the commit version (CV) of the last write that
  covered it, ordered by key in a B+ tree.
- **Reads:** a read range at read version RV conflicts when it meets an interval with a CV above
  RV. Reads are checked one at a time (`checkReads`).
- **Writes:** each accepted write range overwrites the intervals it covers and keeps the parts
  outside it (`addWrites`).
- **Memory by capacity:** nodes (`BNodeStore`) and keys (`KeyStore`) take fixed-size blocks from
  one pool (`PagePool`). When a write needs room, the oldest history is reclaimed and the
  boundary `reclaimCV` advances; a read older than the boundary is rejected as too old instead of
  being checked against history that is gone.
- **Blocks:** 2 KiB by default (`RESOLVER1_BLOCK_SIZE`).

## Standalone build and tests

resolver1 builds and tests on its own, without FoundationDB:

```sh
cd fdbserver/resolver/resolver1
make test                  # C++20 compiler; 2 KiB blocks
make BLOCK_SIZE=4096 test  # other block sizes
bash tests/coverage.sh     # gcov and llvm-cov coverage of PagePool, KeyStore and BNodeStore
```

The validation reports of the components are in `tests/*Tests.md`. `tests/coverage.sh`
regenerates the annotated coverage reports in `tests/evidence/`, which are not tracked.

## FoundationDB integration

- `fdbserver/resolver/BTreeV1.cpp` compiles resolver1's sources inside namespace `btree_v1`.
  The only change to them inside FDB is the `RESOLVER1_IN_FDB` branch of `fdbcompat.h`, which
  uses FDB's `StringRef`.
- `fdbserver/resolver/ConflictSet.cpp` resolves every batch of the binary with btree-v1: the read
  check, the insertion of the combined writes and the end of the batch call `TempConflictBTree`
  directly. The rest of `ConflictBatch` is FoundationDB's.
- `--knob_paged_resolver_pool_pages=N` sets btree-v1's pool to N pages of 64 KiB (16,384 by
  default, 1 GiB). The pool is allocated whole when the Resolver's conflict set is built.
- The Resolver logs `ResolverConflictHistory` with `Index=btree-v1` and the pool's pages.

FoundationDB's own skip list, the baseline of the comparison, is the binary built at the
harness commit `1dfa7ffe6a5210c66e36a628552d10a7046649d0`, the parent of the integration.

## Evaluation

- `fdbserver/resolver/by5eval/` reproduces the comparison of btree-v1 and FDB's skip list at
  equal memory (6 GiB of history): build, accounting and timing runs, and analysis.
- `fdbserver/resolver/by5gen/` holds the trace generator and the hashes of the traces used.
- `fdbserver -r by5resolvertest` is the harness that replays a trace through the Resolver's
  batch path.

## Documentation

The design and evaluation documents of resolver v1 will be published in
<https://github.com/ricardojimenezperis/fdb-beyond-5s>. The link to their v1 snapshot is pending:
that snapshot is not public yet.

## Author

Ricardo Jimenez-Peris

- Website: <https://ricardojimenezperis.com>
- GitHub: <https://github.com/ricardojimenezperis>
