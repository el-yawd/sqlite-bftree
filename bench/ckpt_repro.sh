#!/bin/sh
# OPEN BUG (found 2026-09-15, pre-existing at b2ed3c8): a mid-session
# PRAGMA wal_checkpoint leaves the connection unable to write.
#
#   - the next INSERT fails with "database disk image is malformed";
#   - the database on disk is fine: integrity_check passes immediately after the
#     checkpoint, reads return correct data, and reopening shows a consistent db;
#   - it needs the table to span more than one leaf (400 rows fine, 800 not);
#   - it is specific to the record cache: PRAGMA bf_cache=off makes it go away,
#     and stock SQLite is unaffected;
#   - deferring the checkpoint out of the commit path does NOT help, so it is
#     not the wal-hook re-entrancy it first looked like.
#
# Why it matters beyond the pragma: this is what forces
# sqlite3_wal_autocheckpoint(db, 0) in main.c, which leaves the WAL growing for
# the life of the connection.  walFindFrame was 21% of an insert profile.
#
# Usage: sh bench/ckpt_repro.sh [../build/sqlite3_buf]
BIN="${1:-../build/sqlite3_buf}"
cd "$(dirname "$0")"
DB=$(mktemp -u /tmp/bf_ckpt_repro.XXXX.db)
trap 'rm -f "$DB" "$DB-wal" "$DB-shm"' EXIT

for n in 400 800; do
  rm -f "$DB" "$DB-wal" "$DB-shm"
  out=$("$BIN" "$DB" "
PRAGMA journal_mode=wal;
CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);
INSERT INTO t SELECT value,'v'||value FROM generate_series(1,$n);
PRAGMA wal_checkpoint(PASSIVE);
INSERT INTO t VALUES(999999,'x');
SELECT 'rows='||count(*)||' max='||max(a) FROM t;
PRAGMA integrity_check;" 2>&1)
  rows=$(echo "$out" | grep -o 'rows=[0-9]*' | cut -d= -f2)
  want=$((n+1))
  if [ "$rows" = "$want" ]; then
    echo "n=$n  OK   (rows=$rows)"
  else
    echo "n=$n  BUG  (rows=$rows, expected $want)  $(echo "$out" | grep -i malformed | head -1)"
  fi
done
