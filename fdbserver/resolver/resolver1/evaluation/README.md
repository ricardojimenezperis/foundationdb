# Final resolver v3: publication index

This index lists what the final resolver v3 release consists of: the commits, the documents, and the release assets
that hold the evidence too large for the repository.

## Commits

| commit | what it is |
|---|---|
| `55d5f98974b352f3a124439ae86beb0f8b7ca76d` | resolver v3.8, tag `resolver-v3` |
| `f866c98d8585a77280adbf44c72650f16e975f86` | the stale write rank fix: the final production code |
| `18260afe5f80e00ae2b5cd4952f49522c07cbd0e` | its regression test, catalogs and evidence |
| `2a68fc9b90670bea84c7a458e85d582c7f4eb816` | mixed read-write batch tests |
| `dd5da2b6a40395ba46f27e3e0bd3e5dbb8be25f7` | the 232-test catalogs and evidence, the consolidated evaluation and this index |
| this commit | the final AMD long-trace result |

No commit after `f866c98d` changes a production file.

## Documents

| document | what it holds |
|---|---|
| [`REPORT.md`](REPORT.md) | the consolidated evaluation: correctness on three machines, and performance in the published HugeTLB configuration |
| [`../README.md`](../README.md) | resolver1: what it implements, its build, tests and FoundationDB integration |
| [`../tests/PagePoolTests.md`](../tests/PagePoolTests.md), [`KeyStoreTests.md`](../tests/KeyStoreTests.md), [`BNodeStoreTests.md`](../tests/BNodeStoreTests.md), [`TempConflictBTreeTests.md`](../tests/TempConflictBTreeTests.md) | the validation catalogs of the four components |
| [`../tests/evidence/`](../tests/evidence/) | the test outputs and coverage reports behind the catalogs |
| <https://github.com/ricardojimenezperis/fdb-beyond-5s/tree/main/resolver/v3> | resolver v3's design documents and the v3.8 evaluation |

**Performance figures.** Every public performance figure of resolver v3 is measured with its page pool in HugeTLB
(`RESOLVER1_PAGE_MODE=hugetlb`). FDB, btree-v1 and v2 use their native ordinary-memory implementations. Final v3 uses
its published HugeTLB-backed page-pool configuration. The comparison therefore represents the deployed configurations,
not an isolation of page mode from the data-structure design.

## Release assets

The evidence too large for the repository is attached to the `resolver-v3-final` release, one archive per asset. The
release lists each archive's size and SHA-256, and each archive carries its own `SHA256SUMS`.

| asset | contents |
|---|---|
| AMD evaluation | short-benchmark runs, tables and figures, the earlier single by5 pair with v3 in HugeTLB, the write-group session in HugeTLB, manifests and scripts |
| Intel short benchmarks | the session with v3 in HugeTLB: runs, tables, manifests and scripts |
| Intel by5 | the by5 session with v3 in HugeTLB: runs, per-batch logs, manifests and scripts |
| Intel integration | sanitizers, simulation, recovery and mutant runs |
| test matrices | the macOS and Linux matrices of resolver1's 232 tests |
| AMD fix A/B and component reproduction | the stale write rank fix against v3.8 |
| AMD long trace | the two-pair by5 session with v3 in HugeTLB: runs, per-batch logs, HugeTLB records, manifests, scripts and a script that recomputes the results |
| Intel fix validation | the stale write rank fix on Intel: component and `fdbserver` validation, the A/B against v3.8 in HugeTLB, and the initial end-to-end validation of v3.8 |

Large data (binaries, full traces, raw per-batch logs) is not in the repository.
