# Final resolver v3: consolidated evaluation

This report covers the final resolver v3: correctness on three machines, and performance against FoundationDB's skip
list (FDB), btree-v1 and resolver v2.

## 1. What was evaluated

| commit | tree | what it is |
|---|---|---|
| `55d5f98974b352f3a124439ae86beb0f8b7ca76d` | `433edd77accbc70b47bfeaa4301e6da743139992` | resolver v3.8, tag `resolver-v3` |
| `f866c98d8585a77280adbf44c72650f16e975f86` | `03766c182616a192e38fb713ed3b9124ec428b39` | the stale write rank fix; the final production code |
| `18260afe5f80e00ae2b5cd4952f49522c07cbd0e` | `793a3e13cbf282dde84ea7673e423e35e46bbb23` | its regression test, catalogs and evidence; no production file differs from `f866c98d` |
| `2a68fc9b90670bea84c7a458e85d582c7f4eb816` | `c2b47f360d39af8e5728867b37dcd99af5f746ab` | mixed read-write batch tests; no production file differs from `f866c98d` |

The measured binaries were built from `18260afe`. Their production code is `f866c98d`'s. `2a68fc9b` adds tests only,
so the measurements apply to it unchanged.

**Page mode.** Every time or cost figure of v3 here is measured with its page pool in HugeTLB
(`RESOLVER1_PAGE_MODE=hugetlb`), with the whole pool in huge pages in every run. Correctness tests also run in other
page modes, small pools and sanitizer builds, which exercise paths the performance configuration does not.

**Deployed configurations.** FDB, btree-v1 and v2 use their native ordinary-memory implementations. Final v3 uses its
published HugeTLB-backed page-pool configuration. The comparison therefore represents the deployed configurations, not
an isolation of page mode from the data-structure design.

**Machines.** Every comparison is made within one machine, one core and one session. Figures from different machines
are never compared.

| machine | CPU | used for |
|---|---|---|
| AMD | Ryzen 7 7700, core 7 with its SMT sibling idle | short benchmarks, by5 replica, fix A/B, component reproduction, Linux test matrix |
| Intel | Core Ultra 9 285K, one core, no SMT | short benchmarks, by5 long trace, FoundationDB integration: sanitizers, simulation, recovery, mutants |
| Mac | Apple M3 Pro | resolver1's tests, coverage and catalogs, and the macOS test matrix |

## 2. Correctness

### 2.1 resolver1's tests

- **The suite** has 232 tests: 15 PagePool, 41 KeyStore, 48 BNodeStore and 128 TempConflictBTree.
- **Two matrices, each of 24 configurations, all 232 of 232.** Both cross the same two axes:
  - write tasks: 2 groups of 8 and 2 groups of 4;
  - blocks of 1, 2, 4 and 8 KiB.

| matrix | toolchain | builds |
|---|---|---|
| Mac | macOS 27.0.1, arm64 (Apple M3 Pro), Apple clang 15.0.0 | assertions (`-g`), `-O2 -DNDEBUG`, and UBSan with Guard Malloc (`-g -O1 -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=undefined`, run with `libgmalloc`) |
| AMD (Linux) | clang with libc++ | assertions (`-g -O1`), `-O2 -DNDEBUG`, and ASan/UBSan (`-fsanitize=address,undefined -fno-sanitize-recover=all`) |

- **Sanitizers and warnings.** No sanitizer report and no warning in either matrix. AddressSanitizer does not start
  under Apple clang 15 on that macOS, so ASan runs on Linux only.
- **Mixed batches.** `2a68fc9b`'s cases mix admitted, conflicting and too-old transactions in one batch, and check
  empty, adjacent and overlapping write ranges.
- **Catalogs.** Each component's catalog in [`../tests/`](../tests/) gives every case, the coverage, the mutation checks
  and what is not covered: `PagePoolTests.md`, `KeyStoreTests.md`, `BNodeStoreTests.md`, `TempConflictBTreeTests.md`.

### 2.2 The stale write rank fix, at component level (AMD)

The case is a pool of 2 pages with writes at CV 1 to 60.

| commit | tasks | suite | two-page case | sweep of 1 to 400 writes after the history |
|---|---|---|---|---|
| `55d5f98974` | 2 × 8 | 228/228 | 60 accepted writes lost | 17 sizes bad |
| `55d5f98974` | 2 × 4 | 228/228 | aborts at CV 21 | all 400 abort |
| `f866c98d` | 2 × 8 and 2 × 4 | 229/229 | passes, nothing lost | 0 bad |

- **Builds.** Each row is the same with assertions, `-O2 -DNDEBUG` and ASan/UBSan, except that three aborts become
  losses under `NDEBUG`.
- **Decisions.** With the fix, decisions change only where the unpatched commit was wrong.

### 2.3 FoundationDB integration (Intel)

These runs use `fdbserver` built from `18260afe`, with v3's pool in ordinary memory. They are correctness results, and
correctness may run in any page mode (§1). The integration with v3 in HugeTLB is the by5 long trace (§2.4, §3.4).

| item | what ran | result |
|---|---|---|
| ASan/UBSan | the `/fdbserver/conflictset/` unit tests; a replay against FDB's skip list with pools of 1,024, 4 and 2 pages; the common workload's small-pool cell with 4 and 2 pages, on one and on two Resolvers | all pass. 0 decision mismatches against FDB once the transactions v3 rejects for capacity are excluded, and every `too_old` is explained by the window or the boundary. No `ERROR: AddressSanitizer` and no `runtime error` |
| recovery | a soak of 8 clients with idempotent operations, seeds 11 to 13, v3 and FDB; kills of the Resolver, commit proxy and TLog; 1 ↔ 2 Resolvers; 1 → 2 commit proxies; and the four combined | 46 of 46 runs pass every oracle item: no acknowledged operation lost, no duplicate, the invariants hold, and v3 builds a new `btree-v3` history after each recruitment |
| simulation | 38 upstream specs × seeds 2001 to 2010, normal, buggify and standard modes; 380 runs per arm; v3 with assertions and every batch also resolved by FDB's skip list | 378 of 380 in both arms, with the same two `IncrementTest` failures and the same `RandomUnseed` as unmodified FDB. 0 mismatches against FDB in 4,830,024 transactions |
| mutants | 11 mutants of the integration and of `TempConflictBTree`, each run against the common workload: 12 deterministic scenarios, the soak with 4 to 32 clients, and the small-pool cell | 6 of 11 detected, all as reported failures. M5, M6, M8, M9 and M11 are not detected. M6's site is reached, so the workload's oracles miss its effect |

**Not covered:**
- recovery times as a performance figure (v3 ran in ordinary memory);
- reclamation in the simulation matrix, whose default pool never reclaims (the sanitizer runs cover it);
- directed cases for the five undetected mutants;
- the HugeTLB pool during recovery;
- several machines, network partitions and simultaneous faults.

### 2.4 Logical results of the long trace

Intel's six by5 runs and AMD's four (§3.4, §3.5), FDB and v3 in HugeTLB, give the same results:
- offered hash `11334b1aea22e641` and decisions hash `5001114085b550fb`;
- per-batch decisions identical in all 172,800 batches, across all ten runs and both machines;
- no transaction rejected for the window or for memory;
- v3's boundary and retained history identical, batch by batch, on both machines.

`divergent` is non-zero only in batches 0 to 44 (376 in all), identically in every arm. The run starts from a cold
history, and the reads of those batches, 1.5625 s old, reach versions of the prehistory that is not loaded. They are
common model/trace divergences of the cold start, not differences between FDB and v3.

## 3. Performance

Every v3 figure in this section is v3 in HugeTLB, with the whole pool in huge pages. FDB, btree-v1 and v2 run in
ordinary memory (§1).

### 3.1 Short benchmarks (AMD)

- **Session.** One session; core 7 with CPU 15 idle.
- **Order.** One excluded warm-up per arm, then 5 rotated rounds.
- **Ratios.** Paired within each round, given as median (min–max) of 5.
- **Decisions.** Every timed run gave the published decisions.

| load | v3/v2 | v3/FDB | v3/v1 |
|---|---|---|---|
| conflictset points | 0.7990 (0.7929–0.8026) | 1.0515 (1.0446–1.0572) | 0.5961 (0.5948–0.5978) |
| conflictset ranges | 1.0566 (1.0495–1.0657) | 1.2867 (1.2820–1.2906) | 1.3283 (1.2937–1.3533) |
| macBenchmark points | 0.8416 (0.8348–0.8490) | 1.3237 (1.3115–1.3312) | 0.5255 (0.5201–0.5333) |
| macBenchmark ranges | 0.9253 (0.9172–0.9303) | 0.9899 (0.9848–0.9941) | 0.6594 (0.6519–0.6663) |
| standalone points | 0.8643 (0.8536–0.8788) | N/A | not comparable: 0.8275 (0.8196–0.8370) |
| standalone ranges | 0.8673 (0.8563–0.8777) | N/A | not comparable: 1.0874 (1.0781–1.1038) |
| skiplisttest head16 | 0.8076 (0.8062–0.8162) | 0.9707 (0.9682–0.9819) | 0.5582 (0.5534–0.5641) |
| skiplisttest head32 | 0.7957 (0.7899–0.8039) | 1.0055 (0.9993–1.0133) | 0.5551 (0.5512–0.5628) |
| skiplisttest tail32 | 0.8098 (0.8032–0.8158) | 1.0099 (0.9938–1.0126) | 0.5730 (0.5682–0.5748) |
| skiplisttest head64 | 0.7673 (0.7647–0.7758) | 0.9648 (0.9562–0.9723) | 0.5377 (0.5318–0.5410) |
| skiplisttest tail64 | 0.8112 (0.8060–0.8149) | 0.9531 (0.9415–0.9614) | 0.5922 (0.5874–0.5988) |
| skiplisttest head128 | 0.7407 (0.7271–0.7519) | 0.9869 (0.9628–0.9908) | 0.5353 (0.5185–0.5471) |
| skiplisttest tail128 | 0.8317 (0.8115–0.8510) | 0.9750 (0.9712–0.9953) | 0.6673 (0.6638–0.6863) |

Standalone: there is no FDB arm for that executable, so v3/FDB is N/A. v3/v1 is not comparable: v3's `totalS`
includes its preparation phase (`prepS`) and v1's benchmark has none.

**Reading the table:**
- **Against v2,** v3 takes less time in every load except conflictset ranges, where it takes 5.7% more.
- **Against FDB,** v3 is within −4.7% to +1.0% in skiplisttest and at 0.99 in macBenchmark ranges. It takes more time
  in conflictset points (1.05), conflictset ranges (1.29) and macBenchmark points (1.32).
- **Noise.** On this machine, byte-identical binaries have differed by about ±1%. Ratios within that band are not
  attributed to the code.

### 3.2 Short benchmarks (Intel)

The arms, loads and order are those of §3.1, built from the same trees. The standalone binaries are byte-identical to
AMD's.
- **Session.** 2026-11-09, 00:10 to 02:19, one core with no SMT, unprivileged: no cache drop and no compaction.
- **HugeTLB.** All 78 v3 runs held the whole pool in huge pages: 1,048,576 kB, or 16,777,216 kB for skiplisttest. The
  222 runs of the other arms used none.
- **Decisions.** All 300 runs, warm-ups included, exited 0 with the published decisions.

| load | v3/v2 | v3/FDB | v3/v1 |
|---|---|---|---|
| conflictset points | 0.7198 (0.7193–0.7208) | 0.8078 (0.8056–0.8103) | 0.4977 (0.4965–0.4982) |
| conflictset ranges | 1.0918 (1.0851–1.0935) | 1.1238 (1.1210–1.1263) | 1.1996 (1.1927–1.2045) |
| macBenchmark points | 0.7372 (0.7354–0.7376) | 0.9989 (0.9905–1.0030) | 0.3786 (0.3781–0.3796) |
| macBenchmark ranges | 0.8849 (0.8774–0.8928) | 0.9154 (0.9078–0.9189) | 0.6019 (0.6001–0.6048) |
| standalone points | 0.8201 (0.8167–0.8209) | N/A | not comparable: 0.6368 (0.6344–0.6378) |
| standalone ranges | 0.8903 (0.8895–0.8908) | N/A | not comparable: 1.0068 (1.0028–1.0100) |
| skiplisttest head16 | 0.6892 (0.6858–0.6916) | 0.7354 (0.7341–0.7374) | 0.4216 (0.4201–0.4217) |
| skiplisttest head32 | 0.6754 (0.6738–0.6765) | 0.8155 (0.8145–0.8177) | 0.4274 (0.4271–0.4285) |
| skiplisttest tail32 | 0.7129 (0.7110–0.7147) | 0.8411 (0.8389–0.8431) | 0.4705 (0.4697–0.4713) |
| skiplisttest head64 | 0.6559 (0.6518–0.6568) | 0.8037 (0.7991–0.8054) | 0.4129 (0.4051–0.4133) |
| skiplisttest tail64 | 0.7582 (0.7559–0.7605) | 0.8708 (0.8701–0.8739) | 0.5270 (0.5256–0.5289) |
| skiplisttest head128 | 0.6426 (0.6404–0.6450) | 0.8255 (0.8244–0.8300) | 0.4033 (0.4016–0.4054) |
| skiplisttest tail128 | 0.8441 (0.8371–0.8482) | 0.9103 (0.9075–0.9151) | 0.6392 (0.6345–0.6426) |

Standalone: as in §3.1.

**Reading the table:**
- **Against v2,** v3 takes less time in every load except conflictset ranges, where it takes 9.2% more.
- **Against FDB,** v3 takes less time in conflictset points (0.81), macBenchmark ranges (0.92) and every skiplisttest
  configuration (0.74 to 0.91). It is level in macBenchmark points (0.9989) and takes more time in conflictset ranges
  (1.12).
- **Noise.** This session had no A/A control. Ratios within about ±1% are not attributed to the code. The ranges are
  min–max of five paired ratios, not confidence intervals, and no claim of significance is made.

### 3.3 The fix against v3.8 (AMD)

This compares `f866c98d` with `55d5f98974`:
- both in HugeTLB, 16 measured AB/BA pairs per block;
- loads: blocksizebench points and ranges at 1,024 MiB and at 12 MiB (with reclamation), and skiplisttest head16 and
  head32.

Every paired median is within −0.49% to +0.60%. An A/A control of byte-identical copies shows differences of the same
size, so no regression above 1% is detected, and differences below 1% are not attributed to the fix.

### 3.4 by5 long trace (Intel)

One session on one core: a warm-up of each arm, excluded, then two pairs, FDB → v3 and v3 → FDB.

| check | FDB | v3 |
|---|---|---|
| exit; offered and decisions hashes, all six runs | 0; `11334b1aea22e641`, `5001114085b550fb` | the same |
| HugeTLB | 0 huge pages | 6,291,456 kB (3,072 huge pages) held by the process for the whole run, in each v3 run: the whole pool |
| measured part: accepted, conflicts, target | 19,517,276; 143,524; 143,524 | the same |
| measured part: rejected for the window or for memory; divergent | 0; 0; 0 | the same |
| per-batch decisions, all 172,800 batches | — | equal to FDB's in every batch, in every run |
| retained history, measured part | 265 s by configuration | 582.9 to 584.7 s; boundary and retention identical, batch by batch, to AMD's v3 runs |

| pair | order | FDB `totalS` | v3 `totalS` | v3/FDB |
|---|---|---:|---:|---:|
| 1 | FDB → v3 | 69.612 s | 53.710 s | 0.7716 |
| 2 | v3 → FDB | 69.389 s | 53.646 s | 0.7731 |

- **The two pairs agree within 0.2%.** Their median, 0.7723, rests on two pairs only and carries no ±1% claim.
- **By phase,** v3's read phase takes 1.89 to 1.91 times FDB's, and its write phase 0.43 times.
- **The FDB warm-up** overlapped a read-only filesystem walk on other CPUs. It is excluded, as every warm-up is.

**Workload.**
- Trace: `pilot-2000M-l0-7200.trace`, 15,099,494,528 B, SHA-256
  `0c0d343969fe2e91ef95c09b765b74d078bc9ed48ccf2df15d9dd9db3fd4b6c2`.
- Segment: [57,600, 230,400) from a cold history; the measured part is the last 9,600 batches (300 s).
- FDB: window 265 s.
- v3: no window, a pool of 98,300 pages of 64 KiB (6,442,188,800 B).

### 3.5 by5 long trace, functional and retention replica (AMD)

One session: one warm-up of each arm, excluded, then one pair, FDB then v3. Workload as in §3.4.

| check | FDB | v3 |
|---|---|---|
| exit; offered and decisions hashes | 0; `11334b1aea22e641`, `5001114085b550fb` | the same |
| HugeTLB | — | the whole 6,442,188,800-byte pool in huge pages, in both v3 runs |
| measured part: accepted, conflicts, target | 19,517,276; 143,524; 143,524 | the same |
| measured part: rejected for the window or for memory; divergent | 0; 0; 0 | the same |
| per-batch decisions, all 172,800 batches | — | equal to FDB's in every batch |
| reclamation boundary | — | never decreases; first advances at batch 22,492; always below the commit version |
| retained history, measured part | 265 s by configuration | 582.9 to 584.7 s, median 583.8 s |
| `totalS`, a single-pair observation | 60.500 s | 57.061 s |

This is a functional and retention replica. A single pair supports no precise time difference, so none is inferred from
the two `totalS` values.

**Retention over the segment.** Taken in slices of 9,600 batches, v3's median retained history is 668.6 s in the
slice where the boundary first advances (batches 19,200 to 28,799). It then declines steadily, slice by slice, to
583.8 s in the last one. This is measured; its cause is not
established.

### 3.6 Write-task groups (AMD and Intel)

The published configuration resolves writes with 2 groups of 8 tasks. 2 × 4 and 2 × 6 were measured against it with
the same protocol on both machines:
- the pool in HugeTLB;
- blocksizebench points and ranges, 3,000 batches, 1,024 MiB;
- 24 correctness checks first, then 12 AB/BA pairs per candidate and workload;
- an A/A arm, a byte-identical copy of the 2 × 8 binary.

The candidates are built from `55d5f98974`. This evaluates the write-group shape at that commit, not the final code;
§3.3 bounds the fix's effect at 2 × 8.

A candidate qualifies only with all three of:
- a `totalS` gain of at least 0.5%;
- a median interval below 1;
- an interval below the A/A interval.

Median `totalS` ratio of each candidate to 2 × 8, with its interval:

| machine | workload | 2 × 4 | 2 × 6 | A/A |
|---|---|---|---|---|
| AMD | points | 1.0103 (1.0061–1.0123) | 1.0007 (0.9995–1.0047) | 1.0079 (1.0034–1.0116) |
| AMD | ranges | 1.0116 (1.0063–1.0146) | 1.0081 (1.0057–1.0109) | 1.0033 (0.9997–1.0063) |
| Intel | points | 1.0117 (1.0099–1.0123) | 1.0039 (1.0033–1.0058) | 0.9993 (0.9991–1.0016) |
| Intel | ranges | 1.0027 (1.0014–1.0039) | 1.0012 (1.0000–1.0027) | 0.9989 (0.9972–1.0005) |

- **Result.** No candidate qualifies on either machine, so 2 × 8 stays.
- **Where the cost is.** 2 × 4 takes more time than 2 × 8 everywhere. Its largest phase ratio is the write phase.
- **AMD's points A/A arm** is at 1.0079: the same binary at another path was slower in all 12 pairs. This is a
  placement effect between identical binaries, not code. It widens the envelope a candidate must clear.

## 4. Scope

- **Page mode separated from the data structure** is not measured (§1).
- **conflictset ranges** was not profiled, and the cause of its ratios is not established (§3.1, §3.2).
- **Cold-cache behaviour** is not measured.
