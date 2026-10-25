#!/bin/bash
# Builds a measurement fdbserver from a checkout of this branch and its built tree, as the
# evaluation built its binaries: copies fdbserver/resolver's ConflictSet.cpp, headers, .inc files
# and resolver1/ to OUT, applies the patch of KIND, compiles the copy of ConflictSet.cpp with the
# tree's own command line, puts it in a copy of the tree's libfdbserver_resolver.a and links
# fdbserver with the tree's link line. The checkout is not changed.
#
# usage: build.sh SRC BUILD KIND OUT
#   SRC    checkout: this branch for btree-v1, its harness commit for FDB's skip list
#   BUILD  SRC's build tree, configured and built:
#          cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
#                -DCMAKE_CXX_COMPILER=clang++ -DUSE_LIBCXX=1 -DUSE_LD=LLD SRC && ninja fdbserver
#   KIND   nativemem (timing: logs buffered in memory) | acct (accounting)
#   OUT    new directory for the binary, the patched sources and MANIFEST
# Run it with the toolchain that built BUILD (for example inside foundationdb/build), with SRC and
# BUILD at the paths they had when BUILD was configured.
set -eu
SRC=$(cd "$1" && pwd); BUILD=$(cd "$2" && pwd); KIND=$3; OUT=$4
HERE=$(cd "$(dirname "$0")" && pwd)
[ -e "$OUT" ] && { echo "exists: $OUT"; exit 1; }
[ -z "$(git -C "$SRC" status --porcelain)" ] || { echo "checkout not clean: $SRC"; exit 1; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
cp "$SRC"/fdbserver/resolver/ConflictSet.cpp "$SRC"/fdbserver/resolver/*.h "$SRC"/fdbserver/resolver/*.inc "$OUT"/
cp -r "$SRC"/fdbserver/resolver/resolver1 "$OUT"/resolver1
case $KIND in
 nativemem) python3 "$HERE"/by5-membuf.py "$OUT";;
 acct) python3 "$HERE"/by5-acct.py "$OUT";;
 *) echo "kind?"; exit 1;;
esac
cd "$BUILD"
ninja -t commands bin/fdbserver > "$OUT"/ninja-commands.txt
line=$(grep -E -- "-c $SRC/fdbserver/resolver/ConflictSet\.cpp\$" "$OUT"/ninja-commands.txt | head -1)
[ -n "$line" ] || { echo "no command for ConflictSet.cpp"; exit 1; }
compile=$(echo "$line" | sed -e "s# -o [^ ]*# -o $OUT/ConflictSet.cpp.o#" -e "s# -c [^ ]*# -c $OUT/ConflictSet.cpp#")
link=$(grep -E -- " -o bin/fdbserver( |\$)" "$OUT"/ninja-commands.txt | head -1)
[ -n "$link" ] || { echo "no link command"; exit 1; }
link=$(echo "$link" | sed -e "s#lib/libfdbserver_resolver.a#$OUT/libfdbserver_resolver.a#g" -e "s# -o bin/fdbserver# -o $OUT/fdbserver#")
echo "$compile" > "$OUT"/compile.txt; echo "$link" > "$OUT"/link.txt
eval "$compile"
cp lib/libfdbserver_resolver.a "$OUT"/libfdbserver_resolver.a
(cd "$OUT" && llvm-ar r libfdbserver_resolver.a ConflictSet.cpp.o)
eval "$link"
{ echo "kind=$KIND source=$(git -C "$SRC" rev-parse HEAD) build=$BUILD"; (cd "$HERE" && sha256sum by5-membuf.py by5-acct.py); (cd "$OUT" && sha256sum ConflictSet.cpp *.h *.inc fdbserver); } > "$OUT"/MANIFEST
echo "built $OUT/fdbserver $(sha256sum "$OUT"/fdbserver | cut -d' ' -f1)"
