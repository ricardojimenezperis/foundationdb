#!/usr/bin/env python3
"""Extract MC/DC decisions from `llvm-cov show -show-mcdc` text and check the
per-file totals against `llvm-cov export` JSON.

Usage: mcdc_extract.py <show.txt> <export.json> <out-prefix>

Writes <out-prefix>-records.tsv (one row per decision record, that is, per
instantiation) and <out-prefix>-conditions.tsv (one row per condition after
merging the records of one source decision), and prints the per-file totals.

Merging: llvm-cov reports each template instantiation of a decision as its
own record. A condition of a source decision is counted once; it has an
independence pair if any record of that decision shows one, it is constant
folded if every record shows it folded, and otherwise it has no pair. The
pair is written as its two vectors, from the first record that shows one.
The executed vectors listed for a merged condition are the union of the
vectors of its records. As in llvm-cov's summary, a constant-folded condition is not
counted among the conditions.
"""
import json, re, sys
from collections import OrderedDict

show, export, prefix = sys.argv[1:4]
file_re = re.compile(r'^(\S.*\.(?:cpp|h)):$')
dec_re = re.compile(r'MC/DC Decision Region \((\d+):(\d+)\) to \((\d+):(\d+)\)')
cond_re = re.compile(r'Condition C(\d+) --> \((\d+):(\d+)\)')
vec_re = re.compile(r'^(\d+) \{ (.*) = ([TF])\s*\}')
pair_re = re.compile(r'C(\d+)-Pair: (covered: \((\d+),(\d+)\)|not covered|constant folded|uncoverable.*)$')

def strip(line):
	# Remove the gutter of nested views: any run of spaces and '|' before the text.
	return re.sub(r'^[ |]*', '', line)

records = []
cur_file = None
cur_func = None
rec = None
for raw in open(show, encoding='utf-8'):
	raw = raw.rstrip('\n')
	m = file_re.match(raw)
	if m:
		cur_file, cur_func, rec = m.group(1), None, None
		continue
	body = strip(raw)
	m = re.match(r'^(_Z\S+|[A-Za-z_][\w:<>,~ ]*\S):$', body.strip())
	if m and 'MC/DC' not in body and 'Condition' not in body:
		cur_func = m.group(1)
	m = dec_re.search(body)
	if m:
		rec = dict(file=cur_file, func=cur_func or '-', region='%s:%s-%s:%s' % m.groups(),
				   line=int(m.group(1)), conds=OrderedDict(), vectors=[], pairs={})
		records.append(rec)
		continue
	if rec is None:
		continue
	m = cond_re.search(body)
	if m:
		rec['conds'][int(m.group(1))] = '%s:%s' % (m.group(2), m.group(3))
		continue
	m = vec_re.match(body)
	if m:
		vals = [v.strip() for v in m.group(2).split(',')]
		rec['vectors'].append(('{' + ' '.join(vals) + ' = ' + m.group(3) + '}'))
		rec.setdefault('byindex', {})[int(m.group(1))] = rec['vectors'][-1]
		continue
	m = pair_re.search(body)
	if m:
		c = int(m.group(1))
		s = m.group(2)
		rec['pairs'][c] = 'covered' if s.startswith('covered') else ('folded' if s == 'constant folded' else s)
		if s.startswith('covered'):
			v = rec.get('byindex', {})
			rec['pairs'][c] = 'covered:%s / %s' % (v[int(m.group(3))], v[int(m.group(4))])
		continue
	if 'MC/DC Coverage for Decision' in body:
		rec = None

with open(prefix + '-records.tsv', 'w') as out:
	out.write('file\tdecision\tfunction\tcondition\tposition\tpair\tvectors\n')
	for r in records:
		for c, pos in r['conds'].items():
			out.write('%s\t%s\t%s\tC%d\t%s\t%s\t%s\n' % (r['file'], r['region'], r['func'], c, pos,
				r['pairs'].get(c, '?'), ' '.join(r['vectors']) or '(none)'))

merged = OrderedDict()
for r in records:
	key = (r['file'], r['region'])
	d = merged.setdefault(key, dict(conds=r['conds'], recs=0, status={}, vectors=[]))
	d['recs'] += 1
	for v in r['vectors']:
		if v not in d['vectors']:
			d['vectors'].append(v)
	for c in r['conds']:
		p = r['pairs'].get(c, '?')
		old = d['status'].get(c)
		if p.startswith('covered'):
			d['status'][c] = p
		elif old is None:
			d['status'][c] = p
		elif not old.startswith('covered') and old == 'folded' and p != 'folded':
			d['status'][c] = p

totals = OrderedDict()
with open(prefix + '-conditions.tsv', 'w') as out:
	out.write('file\tline\tdecision\tconditions\tcondition\tposition\tstatus\tpair\tinstantiations\texecuted_vectors\n')
	for (f, region), d in merged.items():
		t = totals.setdefault(f, [0, 0, 0])
		for c, pos in d['conds'].items():
			s = d['status'][c]
			if s == 'folded':
				t[2] += 1
			else:
				t[0] += 1
			if s.startswith('covered'):
				t[1] += 1
			out.write('%s\t%s\t%s\t%d\tC%d\t%s\t%s\t%s\t%d\t%s\n' % (f, region.split(':')[0], region, len(d['conds']), c,
				pos, 'pair' if s.startswith('covered') else ('constant folded, not counted' if s == 'folded' else 'no pair'),
				s[len('covered:'):] if s.startswith('covered') else '-',
				d['recs'], ' '.join(d['vectors']) or '(none)'))

exp = json.load(open(export))
ref = {f['filename']: f['summary']['mcdc'] for f in exp['data'][0]['files']}
print('%-24s %6s %6s %6s %6s | export: %6s %6s' % ('file', 'conds', 'pair', 'nopair', 'folded', 'count', 'covered'))
T = [0, 0, 0]; R = [0, 0]; ok = True
for f in sorted(set(totals) | {k for k, v in ref.items() if v['count']}):
	t = totals.get(f, [0, 0, 0]); r = ref.get(f, {'count': 0, 'covered': 0})
	print('%-24s %6d %6d %6d %6d | export: %6d %6d' % (f, t[0], t[1], t[0] - t[1], t[2], r['count'], r['covered']))
	ok &= (t[0], t[1]) == (r['count'], r['covered'])
	T = [a + b for a, b in zip(T, t)]; R = [R[0] + r['count'], R[1] + r['covered']]
print('%-24s %6d %6d %6d %6d | export: %6d %6d' % ('TOTAL', T[0], T[1], T[0] - T[1], T[2], R[0], R[1]))
print('MC/DC %.2f%%  records %d  source decisions %d  match export: %s' % (100.0 * T[1] / T[0], len(records), len(merged), ok))
sys.exit(0 if ok else 1)
