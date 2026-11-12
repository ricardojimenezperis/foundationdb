#!/usr/bin/env python3
"""Reproduce the provisional count used before the final extraction.

It keyed each condition by (file, first line of its decision, line and
column of the condition) and merged everything with that key, including
the constant-folded condition. Conditions written inside a macro (assert)
share the position of the macro call, so the conditions of one assert
collapse into one key; distinct decisions that start on the same line and
share condition positions also collapse.
Usage: provisional_count.py <show.txt>
"""
import re, sys, collections
cur = None; st = collections.defaultdict(set); dl = None; conds = {}
for l in open(sys.argv[1]):
	l = l.rstrip('\n')
	m = re.match(r'^(\S+\.(?:cpp|h)):$', l)
	if m: cur = m.group(1); continue
	m = re.search(r'MC/DC Decision Region \((\d+):(\d+)\)', l)
	if m: dl = int(m.group(1)); conds = {}; continue
	m = re.search(r'Condition C(\d+) --> \((\d+):(\d+)\)', l)
	if m: conds[int(m.group(1))] = (int(m.group(2)), int(m.group(3))); continue
	m = re.search(r'C(\d+)-Pair: (.*)$', l)
	if m:
		k = (cur, dl) + conds[int(m.group(1))]
		st[k].add('covered' if m.group(2).startswith('covered') else m.group(2).strip())
unc = [k for k, v in st.items() if 'covered' not in v]
print('keys', len(st), 'keys without a pair', len(unc), 'of them constant folded', sum(1 for k in unc if st[k] == {'constant folded'}))
