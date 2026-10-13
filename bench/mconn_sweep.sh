#!/bin/bash
# Multi-connection oracle sweep (1 writer + K readers, bench/mconn_oracle.c).
#
# Runs every seed in [SEEDS, SEEDE] under 7 pragma configurations with 4 readers,
# plus 8 readers whose ring is 128 KiB, in parallel per seed.  Prints each
# failure and a final count; exit 1 if any run failed.
#
#   cc -O1 -g -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
#      -o build/mconn_buf bench/mconn_oracle.c build/sqlite3.c -lm -lpthread
#   SEEDS=1 SEEDE=12 bash bench/mconn_sweep.sh build/mconn_buf
#
# MCONN_BIGPCT (oracle) passes through.  NOPS defaults to 20000.
BIN=${1:-build/mconn_buf}
SEEDS=${SEEDS:-1}; SEEDE=${SEEDE:-12}; NOPS=${NOPS:-20000}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
CFGS=(""
"PRAGMA bf_cache_size=262144;"
"PRAGMA wal_autocheckpoint=50;"
"PRAGMA bf_cache_size=262144; PRAGMA bf_promotion_rate=100; PRAGMA bf_scan_promotion=100;"
"PRAGMA bf_promotion_rate=100; PRAGMA bf_read_record_cache=0;"
"PRAGMA bf_promotion_rate=100; PRAGMA bf_adaptive_full=2;"
"PRAGMA bf_cache_size=262144; PRAGMA wal_autocheckpoint=20; PRAGMA bf_promotion_rate=100;")
run(){ # tag seed nreaders cfg readercfg
  local d=$W/$1
  local out
  out=$("$BIN" "$d.db" "$2" "$NOPS" "$3" "$4" "$5" 2>&1 | tail -1)
  case "$out" in
    OK*) echo ok > "$d.res" ;;
    *)   echo "FAIL seed=$2 readers=$3 cfg='$4' readercfg='$5': $out" | tee "$d.res" ;;
  esac
  rm -f "$d.db" "$d.db-wal" "$d.db-shm"
}
for seed in $(seq "$SEEDS" "$SEEDE"); do
  ci=0
  for c in "${CFGS[@]}"; do run "s${seed}_c$ci" "$seed" 4 "$c" "" & ci=$((ci+1)); done
  run "s${seed}_r8" "$seed" 8 "" "PRAGMA bf_cache_size=131072;" &
  wait
done
n=$(ls "$W"/*.res | wc -l)
f=$(grep -l FAIL "$W"/*.res 2>/dev/null | wc -l)
echo "mconn sweep: $n runs, $f failed"
[ "$f" -eq 0 ]
