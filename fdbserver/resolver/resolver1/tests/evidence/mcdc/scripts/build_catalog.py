#!/usr/bin/env python3
"""Join the extracted conditions, the per-test vectors and the manual
classification into the condition-by-condition catalog.

Usage: build_catalog.py <mcdc-conditions.tsv> <vectors-by-test.tsv> <out-prefix>
Writes <out-prefix>.tsv (every instrumented condition) and <out-prefix>-uncovered.md
(the conditions without a pair and the constant-folded one, as a Markdown table).
Fails if the classification does not match the conditions without a pair exactly.
"""
import csv, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from classification import ROWS

conds_path, vectors_path, prefix = sys.argv[1:4]
conds = list(csv.DictReader(open(conds_path), delimiter='\t'))
first = {}
for r in csv.DictReader(open(vectors_path), delimiter='\t'):
	k = (r['file'], r['decision'], r['vector'])
	o = int(r['order'])
	if k not in first or o < first[k][0]:
		first[k] = (o, r['test'])
cls = {(f, d, c): (text, k, j) for f, d, c, text, k, j in ROWS}
need = {(r['file'], r['decision'], r['condition']) for r in conds if r['status'] != 'pair'}
if need != set(cls):
	sys.exit('classification mismatch: missing %s extra %s' % (sorted(need - set(cls)), sorted(set(cls) - need)))
def short(t):
	return t.split('/')[-1]
out = open(prefix + '.tsv', 'w')
out.write('file\tline\tdecision\tcondition\tposition\tcounted\tclass\tpair\tfirst_test_for_pair\texecuted_vectors\tjustification\n')
md = ['| Location | Decision, condition | Condition | Observed vectors | Class | Justification |', '|---|---|---|---|---|---|']
counts = {}
for r in conds:
	key = (r['file'], r['decision'], r['condition'])
	counted = 'no' if r['status'].startswith('constant folded') else 'yes'
	if r['status'] == 'pair':
		a, b = [v.strip() for v in r['pair'].split(' / ')]
		ta, tb = first[(r['file'], r['decision'], a)], first[(r['file'], r['decision'], b)]
		klass = 'covered: independence pair'
		tests = '%s: %s; %s: %s' % (a, short(ta[1]), b, short(tb[1]))
		just = '-'
		pair = '%s / %s' % (a, b)
	else:
		text, klass, just = cls[key]
		tests = '-'
		pair = '-'
		md.append('| `%s:%s` | `%s` %s of %s | `%s` | %s | %s | %s |' % (r['file'], r['line'], r['decision'],
			r['condition'], r['conditions'], text.replace('|', '\\|'),
			r['executed_vectors'].replace('} {', '}, {') if r['executed_vectors'] != '(none)' else 'none', klass, just.replace('|', '\\|')))
	counts[(r['file'], klass)] = counts.get((r['file'], klass), 0) + 1
	out.write('\t'.join([r['file'], r['line'], r['decision'], '%s/%s' % (r['condition'], r['conditions']), r['position'],
		counted, klass, pair, tests, r['executed_vectors'], just]) + '\n')
open(prefix + '-uncovered.md', 'w').write('\n'.join(md) + '\n')
files = sorted({f for f, _ in counts})
classes = ['covered: independence pair', 'assert condition', 'unreachable (invariant)', 'defensive', 'LG=0 only',
	'reachability not established', 'constant folded (not counted)']
print('class\t' + '\t'.join(files) + '\ttotal')
for k in classes:
	row = [counts.get((f, k), 0) for f in files]
	print('%s\t%s\t%d' % (k, '\t'.join(map(str, row)), sum(row)))
