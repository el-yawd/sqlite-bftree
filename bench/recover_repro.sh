#!/bin/sh
# D1 oracle: what does BF crash recovery leave behind?
#
# STATUS: CLEAN since 2026-09-22.  Kept as a regression test for the bug it
# found on 2026-09-21: BF crash recovery left a STRUCTURALLY CORRUPT B-tree
#
#   PRAGMA integrity_check  ->  Tree 2 page 2 cell 24: Child page depth differs
#
# with every committed row present (a tree-shape defect, not data loss); stock
# SQLite under the identical crash pattern, and the same workload with a clean
# close, were both clean.  Root cause: WAL payload v1 logged only the target
# leaf, so a recovered mini-page took the LEAF as its table root and the
# checkpoint's materialisation opened a write cursor on it; the first split then
# built a second tree beneath a child of the real root.  Fixed by payload v2,
# which logs [leafPgno, rootPgno, op, key, val] (BF_TREE_V2_PLAN.md D1).
#
# (An earlier version of this header said the corruption was present before any
# checkpoint.  That was wrong: it is the checkpoint's materialisation that
# performs the bad split.)
#
# Broader crash coverage -- multiple tables, rollbacks, torn commits, a crash
# DURING the checkpoint, group commit -- lives in bench/crash_oracle.py.
#
# Three scenarios, each a separate database, each gated on row count AND
# integrity_check:
#
#   A  crash, reopen, SELECT (descends the table, so bfTagLeafRoot runs), then
#      checkpoint.
#   B  crash, reopen, checkpoint immediately with no read first.
#   C  crash, reopen in one process, checkpoint in a separate one.
#
# Usage: sh bench/recover_repro.sh [../build/sqlite3_buf] [nrows]
set -u
BIN="${1:-../build/sqlite3_buf}"
ROWS="${2:-4000}"
cd "$(dirname "$0")"
[ -x "$BIN" ] || { echo "MISSING: $BIN" >&2; exit 2; }

fail=0

# Write the rows in a committed transaction, then die without closing cleanly.
# autocheckpoint=0 keeps the record frames in the WAL; a clean close would
# checkpoint them and defeat the whole test.
crash_load() {
  db=$1
  rm -f "$db" "$db-wal" "$db-shm"
  {
    echo "PRAGMA journal_mode=wal;"
    echo "PRAGMA wal_autocheckpoint=0;"
    echo "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);"
    echo "BEGIN;"
    i=1
    while [ "$i" -le "$ROWS" ]; do
      echo "INSERT INTO t VALUES($i,'v$i-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx');"
      i=$((i+1))
    done
    echo "COMMIT;"
    # Hold the connection open so nothing checkpoints, and give the killer a
    # process to find.
    echo "SELECT 'loaded';"
    i=0
    while [ "$i" -lt 100000 ]; do echo "SELECT 1;"; i=$((i+1)); done
  } > "$db.sql"

  "$BIN" "$db" < "$db.sql" > "$db.load.out" 2>&1 &
  pid=$!
  # Wait for the commit to land, then kill without a clean close.
  n=0
  while [ "$n" -lt 600 ]; do
    grep -q loaded "$db.load.out" 2>/dev/null && break
    sleep 0.1
    n=$((n+1))
  done
  grep -q loaded "$db.load.out" 2>/dev/null || {
    echo "SETUP FAIL: load never reached COMMIT for $db" >&2
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    return 1
  }
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  [ -s "$db-wal" ] || {
    echo "SETUP FAIL: no WAL left behind for $db (was it checkpointed?)" >&2
    return 1
  }
  return 0
}

# Both halves gate.  An earlier version of this script checked only the row
# count and printed "ALL CLEAN" over an integrity_check that said
# "Child page depth differs" -- a test that reports success while showing
# corruption is worse than no test.
report() {
  name=$1; got=$2; ic=$3
  ok=1
  msg="$got/$ROWS rows"
  if [ "$got" != "$ROWS" ]; then
    ok=0
    msg="$msg -- $((ROWS-got)) committed rows LOST"
  fi
  if [ "$ic" != "ok" ]; then
    ok=0
    msg="$msg -- integrity_check: $ic"
  fi
  if [ "$ok" = 1 ]; then
    echo "PASS  $name: $msg"
  else
    fail=$((fail+1))
    echo "FAIL  $name: $msg"
  fi
}

# count + integrity of a recovered database, after the given pre-checkpoint SQL
check() {
  db=$1; pre=$2
  [ -n "$pre" ] && "$BIN" "$db" "$pre" >/dev/null 2>&1
  "$BIN" "$db" "PRAGMA wal_checkpoint(TRUNCATE);" >/dev/null 2>&1
  cnt=$("$BIN" "$db" "SELECT count(*) FROM t;" 2>/dev/null)
  icr=$("$BIN" "$db" "PRAGMA integrity_check;" 2>&1 | head -1)
  echo "${cnt:-0}|${icr:-noresult}"
}

# ---- A: descend the table first, then checkpoint -------------------------
DBA=$(mktemp -u /tmp/bf_rec_A.XXXX.db)
if crash_load "$DBA"; then
  r=$(check "$DBA" "SELECT count(*) FROM t;")
  report "A descend-then-checkpoint" "${r%%|*}" "${r#*|}"
else
  fail=$((fail+1))
fi

# ---- B: checkpoint immediately, no read first ----------------------------
DBB=$(mktemp -u /tmp/bf_rec_B.XXXX.db)
if crash_load "$DBB"; then
  r=$(check "$DBB" "")
  report "B checkpoint-before-any-read" "${r%%|*}" "${r#*|}"
else
  fail=$((fail+1))
fi

# ---- C: same as B, but recovery and checkpoint in separate processes ------
DBC=$(mktemp -u /tmp/bf_rec_C.XXXX.db)
if crash_load "$DBC"; then
  "$BIN" "$DBC" "SELECT 1;" >/dev/null 2>&1      # recovery happens here
  r=$(check "$DBC" "")                            # checkpoint in a new process
  report "C recover-then-checkpoint-separately" "${r%%|*}" "${r#*|}"
else
  fail=$((fail+1))
fi

rm -f /tmp/bf_rec_A.*.db* /tmp/bf_rec_B.*.db* /tmp/bf_rec_C.*.db* \
      /tmp/bf_rec_*.sql /tmp/bf_rec_*.out /tmp/bf_rec_*.ic 2>/dev/null

echo
if [ "$fail" -eq 0 ]; then
  echo "ALL CLEAN: recovered records survive a checkpoint in all three orders."
else
  echo "FAILURES: $fail"
  exit 1
fi
