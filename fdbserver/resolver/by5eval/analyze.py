#!/usr/bin/env python3
# Analysis of the 6 GiB comparison (campaign.sh), for two arms: fdb265 and v1.
# - Retention, from each accounting run's batches.csv: the batch of the first discard (retained
#   history below the time elapsed), the minimum after it, by blocks of 600 s, and over the steady
#   state, batches [76800, 172800) (2,400-5,400 s of the segment).
# - Memory, from each accounting run's acct.csv: the peak of history_bytes against the 6 GiB budget.
# - Native time, from the three rounds: totals and phases over the initial cut (batches before the
#   first discard), the steady state and the whole segment; medians and ranges, and the ratio
#   v1/fdb265 paired by round.
# - The hashes of what each run was offered and decided, which must all agree.
# usage: analyze.py RUNDIR [INITIAL_END]   (RUNDIR has acct-<arm>/ and round<r>-<arm>/; INITIAL_END
#        overrides the initial cut, which by default ends at the first discard of either arm)
import csv, json, re, statistics, sys
from pathlib import Path

R = Path(sys.argv[1])
ARMS = ['fdb265', 'v1']
ROUNDS = [1, 2, 3]
N = 172800
BUDGET = 6442450944
LATE = (76800, 172800)          # 2,400-5,400 s: steady state, FDB's history flattened
TIME = {'add_s', 'sort_s', 'read_s', 'intrabatch_s', 'combine_s', 'write_s', 'cleanup_s', 'total_s', 'cumulative_peak_rss_kib'}


def rows(p):
    return list(csv.DictReader(open(p)))


def hashes(p):
    s = open(p).read()
    return {k: re.search(k + r'=(\w+)', s).group(1) for k in ('offeredHash', 'decisionsHash')}


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


out = {'hashes': {}, 'retention': {}, 'memory': {}, 'rejected': {}}
first_discard = {}
for a in ARMS:
    d = R / f'acct-{a}'
    out['hashes'][f'{a}_acct'] = hashes(d / 'out.txt')
    acct = rows(d / 'batches.csv')
    assert len(acct) == N, (a, len(acct))
    t = [int(x['cv']) / 1e6 for x in acct]
    ret = [int(x['retained_us']) / 1e6 for x in acct]
    out['rejected'][a] = {'window': sum(int(x['rejected_window']) for x in acct), 'memory': sum(int(x['rejected_memory']) for x in acct)}
    first = next((i for i in range(N) if ret[i] < t[i] - 1e-6), None)
    first_discard[a] = first
    r = {'first_discard_batch': first, 'first_discard_t_s': t[first] if first is not None else None}
    if first is not None:
        m = min(range(first, N), key=lambda i: ret[i])
        r['minimum_after_first_discard'] = {'s': ret[m], 'batch': m, 't_s': t[m]}
        blocks = []
        for b0 in range(0, N, 19200):
            seg = [ret[i] for i in range(max(b0, first), b0 + 19200)]
            if seg:
                blocks.append({'block_s': f'{b0 // 32}-{(b0 + 19200) // 32}', 'min': min(seg), 'p5': pct(seg, .05), 'median': statistics.median(seg)})
        r['blocks'] = blocks
        late = ret[LATE[0]:LATE[1]]
        r['late'] = {'min': min(late), 'p5': pct(late, .05), 'median': statistics.median(late)}
    out['retention'][a] = r
    am = rows(d / 'batches.csv.acct.csv')
    hb = [int(x['history_bytes']) for x in am]
    out['memory'][a] = {'peak_history_bytes': max(hb), 'last_history_bytes': hb[-1], 'budget': BUDGET, 'within_budget': max(hb) <= BUDGET,
                        'max_metadata_bytes': max(int(x['metadata_bytes']) for x in am),
                        'max_vm_rss_kib': max(int(x['vm_rss_kib']) for x in am),
                        'does_not_fit': (d / 'DOES-NOT-FIT.txt').exists()}


def timing(path, initial_end):
    rs = rows(path)
    assert len(rs) == N, (path, len(rs))
    col = lambda c: [float(x[c]) for x in rs]
    w, rd, cl, tot = col('write_s'), col('read_s'), col('cleanup_s'), col('total_s')
    res = {}
    for n, (a, b) in {'initial': (0, initial_end), 'late': LATE, 'whole': (0, N)}.items():
        tt = tot[a:b]
        res[n] = {'total_s': sum(tt), 'read_s': sum(rd[a:b]), 'write_s': sum(w[a:b]), 'cleanup_s': sum(cl[a:b]),
                  'mean_ms': 1000 * statistics.mean(tt), 'p50_ms': 1000 * pct(tt, .5), 'p95_ms': 1000 * pct(tt, .95), 'p99_ms': 1000 * pct(tt, .99)}
    return res


# Initial cut: before any arm discards history
initial_end = int(sys.argv[2]) if len(sys.argv) > 2 else min(v for v in first_discard.values() if v is not None)
out['initial_cut_end_batch'] = initial_end
tim = {}
for a in ARMS:
    for r in ROUNDS:
        d = R / f'round{r}-{a}'
        out['hashes'][f'{a}_round{r}'] = hashes(d / 'out.txt')
        tim[f'{a}_round{r}'] = timing(d / 'batches.csv', initial_end)
        acct = rows(R / f'acct-{a}' / 'batches.csv')
        nat = rows(d / 'batches.csv')
        out.setdefault('native_vs_acct_differing_columns', {})[f'{a}_round{r}'] = [c for c in acct[0] if c not in TIME and any(x[c] != y[c] for x, y in zip(acct, nat))]
out['timing_per_run'] = tim
summary = {}
for a in ARMS:
    for cut in ('initial', 'late', 'whole'):
        for k in ('total_s', 'read_s', 'write_s', 'cleanup_s', 'p50_ms', 'p95_ms', 'p99_ms'):
            vals = [tim[f'{a}_round{r}'][cut][k] for r in ROUNDS]
            summary.setdefault(a, {}).setdefault(cut, {})[k] = {'median': statistics.median(vals), 'min': min(vals), 'max': max(vals)}
out['timing_summary'] = summary
ratios = {}
for cut in ('initial', 'late', 'whole'):
    for a, b in (('v1', 'fdb265'),):
        rel = [tim[f'{a}_round{r}'][cut]['total_s'] / tim[f'{b}_round{r}'][cut]['total_s'] for r in ROUNDS]
        ratios.setdefault(cut, {})[f'{a}/{b}'] = {'median': statistics.median(rel), 'min': min(rel), 'max': max(rel)}
out['time_ratios_paired'] = ratios
(R / 'analysis.json').write_text(json.dumps(out, indent=1))

print('hashes:', sorted({(h['offeredHash'], h['decisionsHash']) for h in out['hashes'].values()}))
print('rejected:', out['rejected'])
print('native vs acct differing non-time columns:', {k: v for k, v in out['native_vs_acct_differing_columns'].items() if v})
print('initial cut: batches 0-%d' % (initial_end - 1))
for a in ARMS:
    m = out['memory'][a]; r = out['retention'][a]
    print(f"{a:7s} memory peak {m['peak_history_bytes']:,} B ({100 * m['peak_history_bytes'] / BUDGET:.2f}% of budget, within={m['within_budget']}, does_not_fit={m['does_not_fit']}), max metadata {m['max_metadata_bytes']:,} B; "
          f"first discard batch {r['first_discard_batch']}" + (f", late retention min {r['late']['min']:.2f} p5 {r['late']['p5']:.2f} median {r['late']['median']:.2f} s" if 'late' in r else ''))
for a in ARMS:
    for cut in ('initial', 'late', 'whole'):
        s = summary[a][cut]
        print(f"{a:7s} {cut:8s} " + ' '.join(f"{k}={s[k]['median']:.3f}({s[k]['min']:.3f}-{s[k]['max']:.3f})" for k in s))
for cut, rs in ratios.items():
    print(cut, ' '.join(f"{k}={v['median']:.3f}[{v['min']:.3f}-{v['max']:.3f}]" for k, v in rs.items()))
