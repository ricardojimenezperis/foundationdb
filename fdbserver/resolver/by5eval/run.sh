#!/bin/bash
# One by5resolvertest run of the 6 GiB comparison: the 7,200 s trace, segment [57600, 230400) from
# a cold start, one process pinned to CPU.
# usage: run.sh ARM BIN OUT CPU
#   ARM  fdb265: FDB's skip list with a window of 265 s, the largest whose history fits in 6 GiB
#        v1:     btree-v1 with a pool of POOL_PAGES pages of 64 KiB (98,300: 6,442,188,800 bytes)
#   BIN  fdbserver built by build.sh: the harness commit's for fdb265, this branch's for v1
# environment: TRACE (required), SEGB (172800), POOL_PAGES (98300), VALIDATE (0)
set -u
ARM=$1; BIN=$2; OUT=$3; CPU=$4
: "${TRACE:?TRACE must name the 7,200 s trace}"
[ -e "$OUT" ] && { echo "exists: $OUT"; exit 1; }
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
case $ARM in
 fdb265) knobs=""; export BY5_WINDOW_US=265000000;;
 v1) knobs="--knob_paged_resolver_pool_pages=${POOL_PAGES:-98300}"; export BY5_WINDOW_US=0;;
 *) echo "arm?"; exit 1;;
esac
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd); cd "$OUT"
export BY5_TRACE=$TRACE BY5_CPU=$CPU BY5_BATCH_LOG=$OUT/batches.csv BY5_SEGMENT_START=57600 BY5_SEGMENT_BATCHES=${SEGB:-172800} \
  BY5_PREHISTORY_BATCHES=57600 BY5_PRIME=0 BY5_FICT_MODEL=0 BY5_VALIDATE=${VALIDATE:-0} BY5_PROCESS_SEED=1
echo "$BIN arm=$ARM cpu=$CPU sha=$(sha256sum "$BIN" | cut -d' ' -f1) kernel=$(uname -r) hugepages=$(cat /proc/sys/vm/nr_hugepages) load=$(cut -d' ' -f1-3 /proc/loadavg) start=$(date -Is) knobs=[$knobs] window_us=$BY5_WINDOW_US segment_batches=$BY5_SEGMENT_BATCHES" > run.txt
echo x:y@127.0.0.1:1 > dummy.cluster
taskset -c "$CPU" "$BIN" -r by5resolvertest -C dummy.cluster -s 1 $knobs > out.txt 2>&1 < /dev/null &
echo $! > pid
wait $!
rc=$?; rm -f pid; echo "end=$(date -Is) exit=$rc" >> run.txt
sync
