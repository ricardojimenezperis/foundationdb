#!/bin/bash
# MC/DC measurement of resolver1 with LLVM/clang 20 on macOS arm64.
# Run from resolver1's directory:
#   bash <scripts>/mcdc.sh <llvm-bin-dir> <outdir> [extra compiler flags...]
# Builds the full suite with MC/DC instrumentation, runs all tests once, and
# writes the llvm-cov report, show and export of the production files and the
# extraction of their conditions to <outdir>. The extra flags select another
# build, for example -DRESOLVER1_WRITE_LG=0.
set -eu
L=$1
mkdir -p "$2"
O=$(cd "$2" && pwd)
shift 2
X=$(cd "$(dirname "$0")" && pwd)
B=build/mcdc-$(basename "$O")
rm -rf "$B" && mkdir -p "$B"
SDK=$(xcrun --show-sdk-path)
SRC="pagepool.cpp keystore.cpp bnodestore.cpp tempconflictbtree.cpp testmain.cpp tests/*.cpp"
FLAGS="-fuse-ld=lld -isysroot \$(xcrun --show-sdk-path) -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 -fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc -fcoverage-compilation-dir=. $*"
IGN='-ignore-filename-regex=(tests/|test\.h|testmain|fdbcompat|ResolverBatchTypes|/usr/|SDKs)'
{
	echo "# compile and link, one command, from resolver1's directory"
	echo "clang++ $FLAGS $SRC -o tests.exe"
	echo "# run"
	echo "LLVM_PROFILE_FILE=all.profraw ./tests.exe"
	echo "# collect"
	echo "llvm-profdata merge -o all.profdata all.profraw"
	echo "llvm-cov report tests.exe -instr-profile=all.profdata -show-mcdc-summary '$IGN'"
	echo "llvm-cov show tests.exe -instr-profile=all.profdata -show-mcdc -show-branches=count '$IGN'"
	echo "llvm-cov export tests.exe -instr-profile=all.profdata '$IGN'"
} > "$O/build-flags.txt"
# shellcheck disable=SC2086
"$L/clang++" -fuse-ld=lld -isysroot "$SDK" -I. -std=c++20 -Wall -Wextra -Wpedantic -O0 \
	-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc -fcoverage-compilation-dir=. "$@" \
	$SRC -o "$B/tests.exe" > "$O/build.txt" 2>&1
LLVM_PROFILE_FILE="$B/all.profraw" "$B/tests.exe" > "$O/tests.txt" 2>&1
"$L/llvm-profdata" merge -o "$B/all.profdata" "$B/all.profraw"
"$L/llvm-cov" report "$B/tests.exe" -instr-profile="$B/all.profdata" -show-mcdc-summary "$IGN" > "$O/llvm-cov-report.txt"
"$L/llvm-cov" show "$B/tests.exe" -instr-profile="$B/all.profdata" -show-mcdc -show-branches=count "$IGN" > "$O/llvm-cov-show.txt"
"$L/llvm-cov" export "$B/tests.exe" -instr-profile="$B/all.profdata" "$IGN" > "$O/llvm-cov-export.json"
(cd "$O" && python3 -I "$X/mcdc_extract.py" llvm-cov-show.txt llvm-cov-export.json mcdc > mcdc-summary.txt)
cat "$O/mcdc-summary.txt"
