#!/bin/bash
# Builds and runs resolver1's suite in 24 configurations inside the
# foundationdb/build:rockylinux9-latest image: write tasks 2x8 and 2x4,
# blocks of 1, 2, 4 and 8 KiB, and assertions, -O2 -DNDEBUG and ASan/UBSan.
# Usage: linux-matrix.sh <outdir>   (outdir holds src/ from git archive)
set -u
OUT=$(cd "$1" && pwd)
IMG=foundationdb/build:rockylinux9-latest
{
  echo "image $IMG $(docker image inspect --format '{{.Id}}' $IMG)"
  echo "kernel $(uname -srm)"
  echo "linux-matrix.sh $(sha256sum "$0" | cut -d' ' -f1)"
  echo "linux-matrix-inner.sh $(sha256sum "$(dirname "$0")/linux-matrix-inner.sh" | cut -d' ' -f1)"
} > $OUT/MANIFEST.txt
docker run --rm -u $(id -u):$(id -g) -v $OUT:$OUT -v "$(cd "$(dirname "$0")" && pwd)":/scripts:ro -e HOME=/tmp \
	-w $OUT/src/fdbserver/resolver/resolver1 $IMG bash /scripts/linux-matrix-inner.sh $OUT
echo "finished $(date -u +%FT%TZ)" >> $OUT/MANIFEST.txt
