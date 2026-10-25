#!/bin/bash
# Watches an accounting run of FDB's skip list: at the first batch whose history_bytes exceeds
# BUDGET (6 GiB), records it in DOES-NOT-FIT.txt and stops the run. A window that does not fit is
# discarded for exceeding the budget; it is not a functional failure. usage: budget-watch.sh RUNDIR
RUN=$1; BUDGET=${BUDGET:-6442450944}; seen=1
until [ -f "$RUN"/batches.csv.acct.csv ]; do sleep 5; grep -q exit= "$RUN"/run.txt 2>/dev/null && exit 0; done
while :; do
  done_=$(grep -c exit= "$RUN"/run.txt 2>/dev/null)
  n=$(wc -l < "$RUN"/batches.csv.acct.csv)
  over=$(tail -n +$((seen+1)) "$RUN"/batches.csv.acct.csv | head -n $((n-seen)) | awk -F, -v b=$BUDGET '$13+0>b {print $1" "$13; exit}')
  seen=$n
  if [ -n "$over" ]; then
    set -- $over
    echo "window discarded for exceeding the memory budget: first history_bytes above $BUDGET = $2 at batch $1 (index within segment); detected $(date -Is); not a functional failure" > "$RUN"/DOES-NOT-FIT.txt
    [ -f "$RUN"/pid ] && kill "$(cat "$RUN"/pid)"
    exit 0
  fi
  [ "$done_" -gt 0 ] && exit 0
  sleep 10
done
