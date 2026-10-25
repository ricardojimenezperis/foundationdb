# Evidence of the resolver v1 evaluation

The evidence is in the archive `resolver-v1-evidence.tar.xz`, 65,365,216 bytes, SHA-256
`6a88e7002d959be09a1286b2a2e7f07976eaf78b4cc3fe8ca72cdff9ed9318fe`, an asset of the
`resolver-v1` release. `SHA256SUMS`, here and in the archive, lists every file in it; the
archive's README is this file as it was at the tag, without this paragraph. That copy, and the
label of `publication-validation/resolver1-standalone.txt`, describe the second standalone run
as one with blocks of 1 KiB, which it was not (see below); the archive is kept unchanged so that
its checksum holds.

## The 6 GiB comparison on Hetzner

`6gib-hetzner/` holds the runs of FDB's skip list (window 265 s) and of btree-v1 (98,300 pages)
from the campaign of 2026-10-23, on the 7,200 s trace, segment [57,600, 230,400) from a cold start.
The campaign also measured other arms, which are not part of this publication; the files here
are its runs of these two arms, and labelled extracts of its logs.

| Run | Binary SHA-256 | Built from |
|---|---|---|
| `acct-fdb265` | `48fdf5a18949528710ec460fc62cf94ab77a14faa281a86b9bd15b6c0e58ec51` | private commit `2656860f23`, accounting build, FDB's skip list selected |
| `round{1,2,3}-fdb265` | `0533993233bd90d6d2f4ec8fb72e971e1b38889b319ae275851d993632f1e0f2` | private commit `2656860f23`, timing build, FDB's skip list selected |
| `acct-v1` | `a9db5854d77efdd0a1e62a8297997f1aca0b16e914796e72703a89bb61710d7a` | private commit `5a07ab1936`, accounting build |
| `round{1,2,3}-v1` | `6451b555a8efb621ae95eebb356ccc2926cf495c239461450feb72f230bd8e4a` | private commit `5a07ab1936`, timing build |

- Each run directory has `run.txt` (configuration: CPU, knobs, environment, times; the binary is
  identified by its SHA-256, its path is removed), `out.txt` (the harness's summary),
  `batches.csv` (one line per batch) and `batches.csv.ages.csv`; accounting runs also have
  `batches.csv.acct.csv`.
- Accounting runs ran at once on CPUs 1 and 3; timing runs one at a time on CPU 7, CPU 15 idle,
  in the order of `progress-extract.log`. Each run was a `docker run --cpuset-cpus=CPU
  --cpuset-mems=0` with the toolchain image, on an AMD Ryzen 7 7700, Linux 6.8.0, with 3,300
  huge pages reserved.
- `analysis-v1-fdb265.txt` and `.json`: `analyze.py runs 12900` over these runs. Its initial cut
  ends at batch 12,900, as in the campaign's own analysis, whose lines for these two arms are in
  `analysis-original-extract.txt`; the figures agree.

Results:

- Decisions: every run was offered the same transactions and decided the same (`offeredHash`
  `11334b1aea22e641`, `decisionsHash` `5001114085b550fb`); no read was rejected by either arm.
- Memory: FDB's history peaked at 6,354,541,376 bytes (98.64% of 6 GiB); btree-v1's is its pool,
  6,442,188,800 bytes.
- Retention: FDB keeps its 265 s window. btree-v1 first discards history at batch 22,492 (703 s
  into the segment); over the steady state (2,400 to 5,400 s) it retains at least 582.94 s,
  median 592.78 s.
- Native time over the whole segment (median of three rounds): FDB 1,043.874 s (reads 309.453 s,
  writes 532.219 s), btree-v1 2,078.613 s (reads 988.079 s, writes 1,011.784 s). Paired by round,
  btree-v1/FDB is 1.991 (1.981 to 2.003) over the whole segment, 1.800 over the initial cut and
  1.988 over the steady state.

## Validation of the publication

`publication-validation/` checks that the binaries of this branch behave as the evaluated ones.
They were built on the same machine with the same toolchain: the FDB baseline at the harness
commit `1dfa7ffe6a5210c66e36a628552d10a7046649d0` and btree-v1 at the integration commit
`c44fd2247be51c26ca48c56dcc0587fb7b54ea04` (`binaries-sha256.txt`, `measurement-binaries/`).

- **Unit tests** (`unit-*.txt`): with both binaries, the 6 tests of `/fdbserver/conflictset/`
  and the test of `/fdbserver/skiplist/` pass.
- **Smoke replay** (`smoke/`, `smoke-compare.txt`), `SCALE=5`, six cases of 8,500 batches:
  - with room (1,024 pages), btree-v1 decides as FDB's skip list and does not reclaim;
  - under pressure (8 pages, keys padded to 609 bytes), its admitted transactions are decided as
    by the skip list replaying the same batches without its too-old ones, and every too-old
    transaction is explained by the window or by the boundary;
  - the published btree-v1 binary and the evaluated one (`f94a187c5cb143b6…`, from `5a07ab1936`)
    write identical batch lines in both phases (`progress.log`).
- **The by5 harness against the evaluated campaign** (`short-runs/`, `compare-*.txt`): the first
  30,000 batches of the segment, where btree-v1 fills its pool and starts reclaiming at batch
  22,492, with the timing and accounting builds of both arms. Compared batch by batch with the
  same batches of the evaluated runs, every column of `batches.csv` is identical, except the times,
  peak RSS and `measured`, which marks the last 300 s of a segment and so depends on its length:
  decisions, offered and decided hashes, window, boundary and retained history. In
  `batches.csv.acct.csv` every column that describes the history is identical, including
  history_bytes and the skip list's live bytes, whose walk check agrees in every sample.
- **The campaign script** (`campaign-mechanics/`): `campaign.sh` on a segment of 2,000 batches,
  with the binaries above: both accounting runs, then the three rounds in the order fdb265, v1;
  v1, fdb265; v1, fdb265, all exiting with 0 and deciding alike, and the huge pages reserved
  for the campaign released at its end. `budget-watch.sh` ran beside FDB's accounting run and
  had nothing to stop.
- **resolver1 on its own** (`resolver1-standalone.txt`): `make test` at the final snapshot, with
  Clang 19.1.5 on Linux, passes its 193 tests with its fixed blocks of 2 KiB, the only
  configuration tested. The file records two runs; the second, labelled `BLOCK_SIZE=1024`, is a
  duplicate 2 KiB run: the snapshot's `makefile` ignores that variable, so it built and tested the
  same configuration again. It is not evidence for any other block size.

`validate.sh`, `compare-runs.py` and `compare-smoke.py` are the scripts that produced these files.

## Coverage reports removed from the source tree

`resolver1-coverage/` keeps the annotated coverage reports (`*.cpp.gcov`, `*-llvm-show.txt`)
that the validation reports in `fdbserver/resolver/resolver1/tests/` describe, as they were at
`aff127286e`; `tests/coverage.sh` regenerates them.

## What is not included

- The 7,200 s trace (15.1 GB). `../by5gen` regenerates it byte for byte; its arguments and SHA-256
  are in `../by5gen/README.md`. The rebuilt generator reproduced the 40 s reference trace exactly.
- The evaluated binaries, identified above by their SHA-256.
- The runs of the same comparison on a second machine (HP, Intel Core Ultra 9 285K), which were
  not available when this archive was made.
