#!/bin/bash
set -u
OUT=$1
export LD_LIBRARY_PATH=$(dirname $(find /usr /opt -name libc++.so.1 2>/dev/null | head -1))
echo "compiler $(clang++ --version | head -1)" >> $OUT/MANIFEST.txt
SRC="pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp testmain.cpp tests/*.cpp"
BASE="-std=c++20 -stdlib=libc++ -I. -include cstdlib -Wall -Wextra -Wpedantic"
declare -A FL
FL[assert]="-g -O1"
FL[ndebug]="-O2 -DNDEBUG"
FL[asan]="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
one() {
	lg=$1 b=$2 bs=$3
	B=lg${lg}x2-$b-bs$bs
	D=$OUT/$B; mkdir -p $D
	flags="$BASE ${FL[$b]} -DRESOLVER1_WRITE_LG=$lg -DRESOLVER1_WRITE_GROUPS=2 -DRESOLVER1_BLOCK_SIZE=$bs"
	echo "$flags" > $D/flags.txt
	clang++ $flags $SRC -o $D/tests > $D/build.txt 2>&1
	brc=$?
	timeout 3600 $D/tests > $D/tests.txt 2>&1
	rc=$?
	echo "$B build=$brc warnings=$(grep -c 'warning:' $D/build.txt) run=$rc summary=[$(tail -1 $D/tests.txt)] fail=$(grep -c '^FAIL' $D/tests.txt) sanitizer=$(grep -cE 'runtime error|AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer' $D/tests.txt)"
}
export -f one; export OUT BASE SRC; export FL_assert="${FL[assert]}" FL_ndebug="${FL[ndebug]}" FL_asan="${FL[asan]}"
for lg in 8 4; do for b in assert ndebug asan; do for bs in 1024 2048 4096 8192; do echo "$lg $b $bs"; done; done; done |
	xargs -P 8 -n 3 bash -c 'declare -A FL=([assert]="$FL_assert" [ndebug]="$FL_ndebug" [asan]="$FL_asan"); one "$@"' _ > $OUT/results.unsorted.txt
sort $OUT/results.unsorted.txt > $OUT/results.txt; rm $OUT/results.unsorted.txt
cat $OUT/results.txt >> $OUT/MANIFEST.txt
