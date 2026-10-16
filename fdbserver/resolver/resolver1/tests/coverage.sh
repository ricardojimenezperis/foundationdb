#!/bin/bash
# Runs the full test suite and collects gcov and llvm-cov coverage for
# PagePool, KeyStore and BNodeStore into tests/evidence/.
# Run from the project root: bash tests/coverage.sh
set -e
make -B test > tests/evidence/keystore-tests.txt 2>&1
cp tests/evidence/keystore-tests.txt tests/evidence/pagepool-tests.txt
cp tests/evidence/keystore-tests.txt tests/evidence/bnodestore-tests.txt
grep -iE "warning|error" tests/evidence/keystore-tests.txt || true; tail -1 tests/evidence/keystore-tests.txt
FLAGS="-I. -std=c++20 -Wall -Wextra -Wpedantic -O0 --coverage"
build_gcov() {
	rm -rf build/coverage && mkdir -p build/coverage
	for f in pagepool keystore bnodestore testmain tests/pagepooltests tests/keystoretests tests/bnodestoretests; do
		g++ $FLAGS -c $f.cpp -o build/coverage/$(basename $f).o
	done
	g++ --coverage build/coverage/*.o -o build/coverage/tests.exe
}
for c in pagepool:PagePool keystore:KeyStore bnodestore:BNodeStore; do
	IFS=: read n t <<< "$c"
	build_gcov
	(cd build/coverage && ./tests.exe /resolver/$t | tail -1)
	gcov -b -c -o build/coverage/$n.o $n.cpp > build/coverage/$n-coverage.txt 2>&1
	mv *.gcov build/coverage/
	cp build/coverage/$n-coverage.txt build/coverage/$n.cpp.gcov tests/evidence/
done
rm -rf build/coverage-llvm && mkdir -p build/coverage-llvm
g++ -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-compilation-dir=. pagepool.cpp keystore.cpp bnodestore.cpp testmain.cpp tests/*.cpp -o build/coverage-llvm/tests.exe
for c in "pagepool:PagePool:keystore|bnodestore|fdbcompat|test" "keystore:KeyStore:pagepool|bnodestore|fdbcompat|test" "bnodestore:BNodeStore:pagepool|^keystore|fdbcompat|test"; do
	IFS=: read n t x <<< "$c"
	LLVM_PROFILE_FILE=build/coverage-llvm/$n.profraw build/coverage-llvm/tests.exe /resolver/$t | tail -1
	xcrun llvm-profdata merge -o build/coverage-llvm/$n.profdata build/coverage-llvm/$n.profraw
	xcrun llvm-cov report build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/$n.profdata "-ignore-filename-regex=($x)" > build/coverage-llvm/$n-llvm-report.txt
	xcrun llvm-cov show build/coverage-llvm/tests.exe -instr-profile=build/coverage-llvm/$n.profdata -show-branches=count "-ignore-filename-regex=($x)" > build/coverage-llvm/$n-llvm-show.txt
	cp build/coverage-llvm/$n-llvm-report.txt build/coverage-llvm/$n-llvm-show.txt tests/evidence/
done
for n in pagepool keystore bnodestore; do head -4 tests/evidence/$n-coverage.txt; done
cat tests/evidence/*-llvm-report.txt | grep -E "^[a-z.]"
