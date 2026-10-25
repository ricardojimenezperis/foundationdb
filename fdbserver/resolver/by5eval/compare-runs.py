#!/usr/bin/env python3
# Compares, batch by batch, the logs of a run of the published binaries with those of the evaluated
# campaign over the same batches: every column of batches.csv except times and peak RSS, and, with
# --acct, every column of batches.csv.acct.csv that describes the history (not the process's RSS,
# allocator or arena figures, which depend on the binary's layout), except measured, which marks the
# last 300 s of the segment and so depends on its length. Prints per column the number of
# differing rows and the first one. usage: compare-runs.py NEW_DIR EVAL_DIR [--acct]
import csv, sys
from pathlib import Path
new, ev = Path(sys.argv[1]), Path(sys.argv[2]); acct = '--acct' in sys.argv
TIME = {'measured', 'add_s', 'sort_s', 'read_s', 'intrabatch_s', 'combine_s', 'write_s', 'cleanup_s', 'total_s', 'cumulative_peak_rss_kib'}
HIST = {'batch', 'live_skip_bytes', 'walked_skip_bytes', 'pool_physical_bytes', 'pool_used_bytes', 'age_bytes', 'partition_bytes',
        'page_table_bytes', 'metadata_bytes', 'read_scratch_bytes', 'write_scratch_bytes', 'history_bytes', 'history_plus_scratch_bytes',
        'skip64_bytes', 'skip128_bytes', 'skip_large_bytes'}
def cmp(fn, keep):
    a = list(csv.DictReader(open(new / fn))); b = list(csv.DictReader(open(ev / fn)))
    n = len(a); assert n <= len(b), (fn, n, len(b))
    cols = [c for c in a[0] if keep(c)]
    assert all(c in b[0] for c in cols), [c for c in cols if c not in b[0]]
    bad = {}
    for c in cols:
        d = [i for i in range(n) if a[i][c] != b[i][c]]
        if d: bad[c] = (len(d), d[0], a[d[0]][c], b[d[0]][c])
    print(f'{fn}: {n} rows, {len(cols)} columns compared, differing columns: {len(bad)}')
    for c, (k, i, x, y) in bad.items(): print(f'  {c}: {k} rows differ, first at row {i}: new {x} evaluated {y}')
    return not bad
ok = cmp('batches.csv', lambda c: c not in TIME)
if acct: ok &= cmp('batches.csv.acct.csv', lambda c: c in HIST)
print('IDENTICAL' if ok else 'DIFFERENT')
