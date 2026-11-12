#!/bin/bash
# Runs every registered test alone with the clang 20 MC/DC build and records,
# for each decision of the production files, the vectors each test executes.
# Usage: attribute_vectors.sh <llvm-bin> <resolver1-dir> <tests.txt> <outdir> <scripts-dir>
# <tests.txt> is the output of the full run; its PASS lines give the names in
# registration order. Output: <outdir>/vectors-by-test.tsv
set -eu
L=$1 R=$2 T=$3 O=$4 X=$5
IGN='-ignore-filename-regex=(tests/|test\.h|testmain|exactmain|fdbcompat|ResolverBatchTypes|/usr/|SDKs)'
mkdir -p $O/work
cd $R
mkdir -p build/exact
$L/clang++ -fuse-ld=lld -isysroot "$(xcrun --show-sdk-path)" -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 \
	-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc -fcoverage-compilation-dir=. \
	pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp $X/exactmain.cpp tests/*.cpp -o build/exact/tests.exe
printf 'order\ttest\tfile\tdecision\tvector\n' > $O/vectors-by-test.tsv
n=0
grep '^PASS ' $T | cut -d' ' -f2 | while read -r t; do
	n=$((n + 1))
	LLVM_PROFILE_FILE=$O/work/one.profraw build/exact/tests.exe "$t" > $O/work/one.txt 2>&1 || { echo "FAILED $t"; exit 1; }
	$L/llvm-profdata merge -o $O/work/one.profdata $O/work/one.profraw
	$L/llvm-cov show build/exact/tests.exe -instr-profile=$O/work/one.profdata -show-mcdc "$IGN" > $O/work/one-show.txt
	python3 -I - $O/work/one-show.txt "$n" "$t" >> $O/vectors-by-test.tsv <<'PY'
import re, sys
show, order, test = sys.argv[1:4]
file_re = re.compile(r'^(\S.*\.(?:cpp|h)):$')
dec_re = re.compile(r'MC/DC Decision Region \((\d+):(\d+)\) to \((\d+):(\d+)\)')
vec_re = re.compile(r'^(\d+) \{ (.*) = ([TF])\s*\}')
seen = set(); f = None; d = None
for raw in open(show):
	raw = raw.rstrip('\n')
	m = file_re.match(raw)
	if m: f = m.group(1); d = None; continue
	body = re.sub(r'^[ |]*', '', raw)
	m = dec_re.search(body)
	if m: d = '%s:%s-%s:%s' % m.groups(); continue
	m = vec_re.match(body)
	if m and d:
		v = '{' + ' '.join(x.strip() for x in m.group(2).split(',')) + ' = ' + m.group(3) + '}'
		if (f, d, v) not in seen:
			seen.add((f, d, v)); print('%s\t%s\t%s\t%s\t%s' % (order, test, f, d, v))
PY
done
rm -rf $O/work
echo "tests attributed: $(cut -f1 $O/vectors-by-test.tsv | sort -u | grep -vc order)"
