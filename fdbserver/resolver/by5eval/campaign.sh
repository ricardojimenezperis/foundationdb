#!/bin/bash
# The 6 GiB comparison of FDB's skip list (window 265 s) and btree-v1 (98,300 pages): one
# accounting run per arm, both at once on ACCT_CPUS (not timed), then three native rounds, one
# run at a time on TIMED_CPU, with the order of the arms alternated, and the page cache drained
# between runs. Needs passwordless sudo for vm.nr_hugepages.
# usage: campaign.sh OUT FDB_BIN_DIR V1_BIN_DIR
#   *_BIN_DIR: directories with nativemem/ and acct/ binaries from build.sh
# environment: TRACE (required), TIMED_CPU (7), ACCT_CPUS ("1 2"), SEGB (172800)
set -u
OUT=$1; FDB=$(cd "$2" && pwd); V1=$(cd "$3" && pwd); HERE=$(cd "$(dirname "$0")" && pwd); R=$HERE/run.sh
: "${TRACE:?TRACE must name the 7,200 s trace}"
TIMED_CPU=${TIMED_CPU:-7}; read -r ACPU1 ACPU2 <<< "${ACCT_CPUS:-1 2}"
[ -e "$OUT" ] && { echo "exists: $OUT"; exit 1; }; mkdir -p "$OUT"; P=$OUT/progress.log
bin() { case $1 in fdb265) echo "$FDB/$2/fdbserver";; v1) echo "$V1/$2/fdbserver";; esac; }
drain() { sync; for i in $(seq 1 60); do d=$(awk '/^(Dirty|Writeback):/{s+=$2} END{print s}' /proc/meminfo); [ "$d" -lt 1024 ] && break; sleep 1; done; echo "drain $(date -Is) dirty+writeback_kib=$d" >> "$P"; }
for i in 1 2 3 4 5; do sudo -n sysctl -w vm.nr_hugepages=0 >/dev/null; sudo -n sysctl -w vm.compact_memory=1 >/dev/null; sleep 3; sudo -n sysctl -w vm.nr_hugepages=3300 >/dev/null; [ "$(awk '/HugePages_Free/{print $2}' /proc/meminfo)" -ge 3300 ] && break; done
{ echo "campaign start $(date -Is) kernel=$(uname -r) nr_hugepages=$(cat /proc/sys/vm/nr_hugepages) trace=$(stat -c %s "$TRACE")";
  for a in fdb265 v1; do for k in nativemem acct; do echo "bin $a-$k $(sha256sum "$(bin $a $k)" | cut -d' ' -f1)"; done; done;
  (cd "$HERE" && sha256sum *.sh *.py); } > "$OUT"/MANIFEST
echo "acct start $(date -Is)" >> "$P"
"$R" fdb265 "$(bin fdb265 acct)" "$OUT"/acct-fdb265 "$ACPU1" & sleep 20; "$HERE"/budget-watch.sh "$OUT"/acct-fdb265 &
"$R" v1 "$(bin v1 acct)" "$OUT"/acct-v1 "$ACPU2" &
wait
for a in fdb265 v1; do echo "acct $a $(tail -1 "$OUT"/acct-$a/run.txt) $([ -e "$OUT"/acct-$a/DOES-NOT-FIT.txt ] && echo DOES-NOT-FIT)" >> "$P"; done
orders=("fdb265 v1" "v1 fdb265" "v1 fdb265")
for r in 0 1 2; do for a in ${orders[$r]}; do
  drain; echo "round$((r+1)) $a start $(date -Is)" >> "$P"
  "$R" $a "$(bin $a nativemem)" "$OUT"/round$((r+1))-$a "$TIMED_CPU"
  echo "round$((r+1)) $a $(tail -1 "$OUT"/round$((r+1))-$a/run.txt) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$P"
done; done
drain; sudo -n sysctl -w vm.nr_hugepages=0 >/dev/null
echo "campaign end $(date -Is) nr_hugepages=$(cat /proc/sys/vm/nr_hugepages)" >> "$OUT"/MANIFEST; echo done >> "$P"
