#!/bin/sh
# Differential stress test: run identical randomized SQL workloads against a
# stock SQLite build (Config A, compiled with -DSQLITE_OMIT_BF_CACHE) and the
# BF fork (BF on by default), then compare query output AND the final .dump
# byte-for-byte.  Any divergence is a stop-the-line correctness bug.
#
# This is the project's primary correctness oracle (plan §1.4).  It must pass
# clean before committing any stage.
#
# Usage:
#   sh bench/stress.sh                # default seed set + journal modes
#   sh bench/stress.sh 5 12 99        # explicit seeds
#   NOPS=8000 sh bench/stress.sh      # heavier workload per seed
#
# Requires (built into ../build):
#   sqlite3        - the BF fork CLI
#   sqlite3_stock  - stock CLI (cc -DSQLITE_OMIT_BF_CACHE ... shell.c sqlite3.c)
set -e
cd "$(dirname "$0")"

BUILD=../build
BF="$BUILD/sqlite3"
STOCK="$BUILD/sqlite3_stock"
GEN="./gen_stress.py"
NOPS="${NOPS:-4000}"
WORKDIR="./stress_work"

for bin in "$BF" "$STOCK"; do
  if [ ! -x "$bin" ]; then
    echo "MISSING: $bin — build it first (see header)." >&2
    exit 2
  fi
done

mkdir -p "$WORKDIR"
rm -f "$WORKDIR"/*

# Default seeds: 18+ as the acceptance checklist requires.
SEEDS="$*"
if [ -z "$SEEDS" ]; then
  SEEDS="1 2 3 5 7 11 13 17 23 42 99 123 777 1024 2024 12345 31337 65535"
fi
JOURNALS="delete wal memory"

fail=0
ntests=0
for seed in $SEEDS; do
  for jm in $JOURNALS; do
    ntests=$((ntests + 1))
    base="$WORKDIR/s${seed}_${jm}"
    sql="${base}.sql"
    python3 "$GEN" "$seed" "$jm" "$NOPS" > "$sql"

    # Run each engine on a fresh DB; capture stdout (query results +
    # integrity_check + ordered dumps) and a full schema+data .dump.
    rm -f "${base}.bf.db" "${base}.st.db"
    "$BF"    "${base}.bf.db" < "$sql" > "${base}.bf.out" 2>&1 || true
    "$STOCK" "${base}.st.db" < "$sql" > "${base}.st.out" 2>&1 || true
    "$BF"    "${base}.bf.db" ".dump" > "${base}.bf.dump" 2>&1 || true
    "$STOCK" "${base}.st.db" ".dump" > "${base}.st.dump" 2>&1 || true

    ok=1
    if ! diff -q "${base}.st.out" "${base}.bf.out" >/dev/null; then ok=0; fi
    if ! diff -q "${base}.st.dump" "${base}.bf.dump" >/dev/null; then ok=0; fi

    if [ "$ok" = 1 ]; then
      echo "PASS  seed=$seed journal=$jm"
    else
      fail=$((fail + 1))
      echo "FAIL  seed=$seed journal=$jm  (see ${base}.*.out / ${base}.*.dump)"
      echo "----- output diff (first 20 lines) -----"
      diff "${base}.st.out" "${base}.bf.out" | head -20 || true
      echo "----- dump diff (first 20 lines) -----"
      diff "${base}.st.dump" "${base}.bf.dump" | head -20 || true
    fi
  done
done

echo ""
if [ "$fail" = 0 ]; then
  echo "ALL CLEAN: $ntests differential tests passed."
else
  echo "DIVERGENCES: $fail of $ntests tests FAILED — stop the line."
  exit 1
fi
