#!/bin/bash
# Threaded BEGIN CONCURRENT oracle sweep (bench/mt_oracle.c).
#
#   cc -O1 -g -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
#      -o build/mt_bf bench/mt_oracle.c build/sqlite3.c -lm -lpthread
#   SEEDS=1 SEEDE=4 bash bench/mt_sweep.sh build/mt_bf
# Every seed under 7 configurations at 4 threads, plus 8 and 16 threads.
BIN=${1:-build/mt_bf}
SEEDS=${SEEDS:-1}; SEEDE=${SEEDE:-4}; NTXN=${NTXN:-1500}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
CFGS=(""
"PRAGMA bf_cache_size=262144;"
"PRAGMA wal_autocheckpoint=50;"
"PRAGMA bf_cache_size=131072; PRAGMA wal_autocheckpoint=20;"
"PRAGMA bf_promotion_rate=100; PRAGMA bf_scan_promotion=100;"
"PRAGMA bf_promotion_rate=100; PRAGMA bf_read_record_cache=0;"
"PRAGMA bf_group_commit=8;")
run(){ # tag seed nthr cfg
  local d=$W/$1 out
  timeout ${TMO:-600} "$BIN" "$d.db" "$2" "$3" "$NTXN" "$4" > "$d.log" 2>&1
  local ec=$?
  out=$(grep -E "^OK|FAIL|Assertion" "$d.log" | head -3 | tr '\n' ' ')
  case "$out" in
    OK*) echo ok > "$d.res" ;;
    *)   echo "FAIL seed=$2 threads=$3 cfg='$4' exit=$ec: ${out:-no output}" | tee "$d.res"
         [ -n "$KEEP" ] && cp "$d.log" "$KEEP/$1.log" ;;
  esac
  rm -f "$d.db" "$d.db-wal" "$d.db-shm"
}
for seed in $(seq "$SEEDS" "$SEEDE"); do
  ci=0
  for c in "${CFGS[@]}"; do run "s${seed}_c$ci" "$seed" 4 "$c" & ci=$((ci+1)); done
  run "s${seed}_t8" "$seed" 8 "" &
  run "s${seed}_t16" "$seed" 16 "PRAGMA bf_cache_size=262144;" &
  wait
done
n=$(ls "$W"/*.res | wc -l)
f=$(grep -l FAIL "$W"/*.res 2>/dev/null | wc -l)
echo "mt sweep: $n runs, $f failed"
[ "$f" -eq 0 ]
