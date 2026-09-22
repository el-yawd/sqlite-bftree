#!/bin/sh
# Cross-table differential oracle, under ring pressure.
#
# Compares ../build/sqlite3_buf against ../build/sqlite3_stock on a workload
# that writes MANY tables inside one transaction with a ring far too small to
# hold them (gen_xtable.py explains why that shape matters).
#
# This is not a variant of stress_buf.sh for completeness' sake: the
# single-table oracles cannot produce a foreign table's dirty mini-page at the
# ring head at all, because the insert fallback flushes the cursor's table on
# the first BF refusal.  Every mechanism that touches eviction, the mapping
# table or the size-class ladder should be gated here as well.
#
# HONEST LIMIT, stated so nobody reads more coverage into this than it has:
# this oracle does NOT reproduce the 2026-09-21 pEvictProtect data-loss bug.
# That was found by a hand-built workload driving M1's stall drain, and with
# M1's same-leaf guard in place the trigger is gone -- a binary with the fix
# reverted passes every case here.  The bug's only DEMONSTRATED trigger was
# flushing a leaf and immediately buffering into it.  Whether it is reachable
# without that remains unproven; see BF_TREE_V2_PLAN.md.
#
# Usage: sh bench/stress_xtable.sh [seeds...]
#        NTAB=16 ROWS=5000 sh bench/stress_xtable.sh
#        BUF=../build/sqlite3_other sh bench/stress_xtable.sh
set -e
cd "$(dirname "$0")"
BUF="${BUF:-../build/sqlite3_buf}"
STOCK="${STOCK:-../build/sqlite3_stock}"
NTAB="${NTAB:-12}"
ROWS="${ROWS:-3000}"
WORK=./stress_work
for bin in "$BUF" "$STOCK"; do
  [ -x "$bin" ] || { echo "MISSING: $bin" >&2; exit 2; }
done
mkdir -p "$WORK"; rm -f "$WORK"/xt_*
SEEDS="$*"
[ -z "$SEEDS" ] && SEEDS="1 7 42 1024 31337"
fail=0; n=0
for seed in $SEEDS; do
  for journal in delete wal memory; do
    tag="xt_s${seed}_${journal}"
    sql="$WORK/$tag.sql"
    python3 gen_xtable.py "$seed" "$journal" "$NTAB" "$ROWS" > "$sql"

    for eng in bf st; do
      case $eng in
        bf) bin=$BUF ;;
        st) bin=$STOCK ;;
      esac
      db="$WORK/$tag.$eng.db"
      rm -f "$db" "$db-wal" "$db-shm"
      "$bin" "$db" < "$sql" > "$WORK/$tag.$eng.out" 2>&1 || true
      "$bin" "$db" .dump > "$WORK/$tag.$eng.dump" 2>&1 || true
    done

    n=$((n+1))
    if cmp -s "$WORK/$tag.bf.out" "$WORK/$tag.st.out" \
    && cmp -s "$WORK/$tag.bf.dump" "$WORK/$tag.st.dump"; then
      echo "PASS  seed=$seed journal=$journal tables=$NTAB"
    else
      fail=$((fail+1))
      echo "FAIL  seed=$seed journal=$journal tables=$NTAB"
      diff "$WORK/$tag.st.out" "$WORK/$tag.bf.out" | head -5 || true
      diff "$WORK/$tag.st.dump" "$WORK/$tag.bf.dump" | head -5 || true
    fi
  done
done

echo
if [ "$fail" -eq 0 ]; then
  echo "ALL CLEAN: $n cross-table differential tests passed."
else
  echo "FAILURES: $fail of $n"
  exit 1
fi
