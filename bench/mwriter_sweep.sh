#!/bin/bash
# Multi-WRITER oracle sweep (BEGIN CONCURRENT, bench/mwriter_oracle.c).
#
# Every seed in [SEEDS, SEEDE] under 7 configurations (4 writers) plus 8 writers,
# in parallel per seed.  MODES picks the oracle checks (default: plain run and
# MW_CHECKCOMMIT, which verifies each committer's whole view after its commit).
#
#   cc -O1 -g -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
#      -o build/mw_bf bench/mwriter_oracle.c build/sqlite3.c -lm -lpthread
#   SEEDS=1 SEEDE=8 bash bench/mwriter_sweep.sh build/mw_bf
# Validate the oracle with the stock_bc build first (BF pragmas are ignored there).
BIN=${1:-build/mw_bf}
SEEDS=${SEEDS:-1}; SEEDE=${SEEDE:-8}; NOPS=${NOPS:-20000}
MODES=${MODES:-"plain checkcommit"}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
CFGS=(""
"PRAGMA bf_cache_size=262144;"
"PRAGMA wal_autocheckpoint=50;"
"PRAGMA bf_cache_size=262144; PRAGMA bf_promotion_rate=100; PRAGMA bf_scan_promotion=100;"
"PRAGMA bf_promotion_rate=100; PRAGMA bf_read_record_cache=0;"
"PRAGMA bf_cache_size=131072; PRAGMA wal_autocheckpoint=20;"
"PRAGMA bf_promotion_rate=100; PRAGMA bf_adaptive_full=2;")
run(){ # tag seed nconn cfg mode
  local d=$W/$1 out env=""
  case "$5" in checkcommit) env="MW_CHECKCOMMIT=1";; norange) env="MW_NORANGE=1";; esac
  out=$(env $env "$BIN" "$d.db" "$2" "$NOPS" "$3" "$4" 2>&1 | grep -E "^OK|FAIL" | head -1)
  case "$out" in
    OK*) echo ok > "$d.res" ;;
    *)   echo "FAIL seed=$2 conns=$3 mode=$5 cfg='$4': $out" | tee "$d.res" ;;
  esac
  rm -f "$d.db" "$d.db-wal" "$d.db-shm"
}
for mode in $MODES; do
  for seed in $(seq "$SEEDS" "$SEEDE"); do
    ci=0
    for c in "${CFGS[@]}"; do run "${mode}_s${seed}_c$ci" "$seed" 4 "$c" "$mode" & ci=$((ci+1)); done
    run "${mode}_s${seed}_w8" "$seed" 8 "" "$mode" &
    wait
  done
done
n=$(ls "$W"/*.res | wc -l)
f=$(grep -l FAIL "$W"/*.res 2>/dev/null | wc -l)
echo "mwriter sweep: $n runs, $f failed"
[ "$f" -eq 0 ]
