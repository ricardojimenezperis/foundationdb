#!/bin/bash
# Builds and runs resolver1's suite in 24 configurations with Apple clang:
# write tasks 2x8 and 2x4, blocks of 1, 2, 4 and 8 KiB, and assertions (-g),
# -O2 -DNDEBUG and UBSan run under Guard Malloc (libgmalloc).
# Usage: mac-matrix.sh <outdir>   (outdir holds src/ from git archive)
set -u
OUT=$(cd "$1" && pwd)
cd $OUT/src/fdbserver/resolver/resolver1
{
  echo "platform $(sw_vers -productName) $(sw_vers -productVersion) $(uname -m) $(sysctl -n machdep.cpu.brand_string)"
  echo "compiler $(clang++ --version | head -1)"
  echo "mac-matrix.sh $(shasum -a 256 "$0" | cut -d' ' -f1)"
} > $OUT/MANIFEST.txt
SRC="pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp testmain.cpp tests/*.cpp"
export OUT SRC
one() {
	lg=$1 b=$2 bs=$3
	case $b in
	assert) fl="-g";;
	ndebug) fl="-O2 -DNDEBUG";;
	ubsan-gmalloc) fl="-g -O1 -fno-omit-frame-pointer -fsanitize=undefined -fno-sanitize-recover=undefined";;
	esac
	B=lg${lg}x2-$b-bs$bs
	D=$OUT/$B; mkdir -p $D
	flags="-std=c++20 -I. -Wall -Wextra -Wpedantic $fl -DRESOLVER1_WRITE_LG=$lg -DRESOLVER1_WRITE_GROUPS=2 -DRESOLVER1_BLOCK_SIZE=$bs"
	echo "$flags" > $D/flags.txt
	clang++ $flags $SRC -o $D/tests > $D/build.txt 2>&1
	brc=$?
	if [ $b = ubsan-gmalloc ]; then
		DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib $D/tests > $D/tests.txt 2>&1
	else
		$D/tests > $D/tests.txt 2>&1
	fi
	rc=$?
	echo "$B build=$brc warnings=$(grep -c 'warning:' $D/build.txt) run=$rc summary=[$(tail -1 $D/tests.txt)] fail=$(grep -c '^FAIL' $D/tests.txt) sanitizer=$(grep -cE 'runtime error|Sanitizer|GuardMalloc\[.*(error|failed)' $D/tests.txt)"
}
export -f one
for lg in 8 4; do for b in assert ndebug ubsan-gmalloc; do for bs in 1024 2048 4096 8192; do echo "$lg $b $bs"; done; done; done |
	xargs -P 6 -n 3 bash -c 'one "$@"' _ | sort > $OUT/results.txt
cat $OUT/results.txt >> $OUT/MANIFEST.txt
echo "finished $(date -u +%FT%TZ)" >> $OUT/MANIFEST.txt
