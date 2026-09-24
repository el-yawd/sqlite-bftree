#!/bin/sh
# D1 oracle: a torn commit whose FIRST frame is a BF record frame.
#
# RESULT, 2026-09-24 -- FOUND A REAL BUG (fixed in wal.c walFrames).  After a
# crash mid-commit, the next write lost one of its own wal-index entries and a
# checkpoint backfilled a stale page:
#
#   PRAGMA integrity_check  ->  Tree 4 page 4 cell 0: invalid page number 151
#
# with the header still saying 150 pages.  Mechanism: recovery indexes every
# valid frame, torn tail included, and stock SQLite clears such a tail lazily --
# walIndexAppend() finds the slot it is about to fill already set and runs
# walCleanupHash().  That relies on the tail being contiguous.  A BF record
# frame occupies a slot WITHOUT indexing it, so when the dead writer's first
# frame was a record frame, the next transaction's first page frame found an
# empty slot, its second frame found the stale entry, and the cleanup -- with
# hdr.mxFrame not yet advanced -- also deleted the entry just appended.
#
# Found by the crash-differential oracle under group commit, where the kill
# landed mid-commit by chance.  This script makes it deterministic: commit a
# transaction whose frames are [record][page images...][commit], crash, then
# cut the commit frame off the WAL.  What remains is exactly that torn tail.
#
# Usage: sh bench/torn_tail_repro.sh [../build/sqlite3_buf]
set -u
BIN="${1:-../build/sqlite3_buf}"
cd "$(dirname "$0")"
[ -x "$BIN" ] || { echo "MISSING: $BIN" >&2; exit 2; }
DB=$(mktemp -u /tmp/bf_torn.XXXX.db)
trap 'rm -f "$DB" "$DB-wal" "$DB-shm" "$DB.sql" "$DB.out"' EXIT

{
  echo "PRAGMA journal_mode=wal;"
  echo "PRAGMA wal_autocheckpoint=0;"
  echo "CREATE TABLE a(id INTEGER PRIMARY KEY, v TEXT);"
  echo "CREATE TABLE b(id INTEGER PRIMARY KEY, v BLOB);"
  # Committed history: a table spanning many leaves (ids 10, 20, ... 4000),
  # most of it in base by the end of the load ...
  i=10
  while [ "$i" -le 4000 ]; do
    echo "INSERT INTO a VALUES($i, 'row-$i-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx');"
    i=$((i+10))
  done
  echo "INSERT INTO b VALUES(1, randomblob(100));"
  # ... then single-row commits spread across that range, so that at the crash
  # BUFFERED committed records sit on several different leaves.  The close-time
  # checkpoint must then materialise at least two pages: the bug needs a second
  # frame to trip the cleanup that deletes the first.
  for k in 5 1005 2005 3005 3995; do
    echo "INSERT INTO a VALUES($k, 'late-$k');"
  done
  # The transaction to tear: buffered rows (-> a record frame, emitted first)
  # plus an overflow blob (-> page images, the last one carrying the commit).
  # The rows go into gaps of existing leaves, so they are buffered (an append
  # past the last leaf takes the base path and would emit no record frame).
  echo "BEGIN;"
  for k in 15 1015 2015 3015; do
    echo "INSERT INTO a VALUES($k, 'torn-$k');"
  done
  echo "INSERT INTO b VALUES(2, randomblob(9000));"
  echo "COMMIT;"
  echo ".print LOADED"
  i=0
  while [ "$i" -lt 100000 ]; do echo "SELECT 1;"; i=$((i+1)); done
} > "$DB.sql"

rm -f "$DB" "$DB-wal" "$DB-shm"
"$BIN" "$DB" < "$DB.sql" > "$DB.out" 2>&1 &
pid=$!
n=0
while [ "$n" -lt 600 ]; do
  grep -q LOADED "$DB.out" 2>/dev/null && break
  sleep 0.1; n=$((n+1))
done
kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
grep -q LOADED "$DB.out" || { echo "SETUP FAIL: load never committed"; exit 1; }

# Tear the last commit: drop its final frame (the one carrying the commit mark).
python3 - "$DB-wal" <<'EOF' || exit 1
import os, struct, sys
p = sys.argv[1]
w = open(p, 'rb').read()
ps = struct.unpack('>I', w[8:12])[0]
fs = 24 + ps
n = (len(w) - 32) // fs
frames = [struct.unpack('>II', w[32 + i*fs: 40 + i*fs]) for i in range(n)]
commits = [i for i, (pg, nt) in enumerate(frames) if nt]
last, prev = commits[-1], commits[-2]
tail = frames[prev+1:last+1]
if tail[0][0] != 0xffffffff or len(tail) < 3:
    sys.exit("SETUP FAIL: last commit is not [record][pages...]: %r" % (tail,))
os.truncate(p, 32 + last*fs)       # keep everything before the commit frame
print("torn tail: %d frames, first is a record frame" % (len(tail) - 1))
EOF

# Recovery, then a clean close: the close-time checkpoint materialises the
# recovered records -- the write that used to lose its own index entry.
"$BIN" "$DB" "PRAGMA integrity_check;" > /dev/null 2>&1
ic=$("$BIN" "$DB" "PRAGMA integrity_check;" 2>&1 | head -2 | tr '\n' ' ')
cnt=$("$BIN" "$DB" "SELECT (SELECT count(*) FROM a)||'/'||(SELECT count(*) FROM b);" 2>&1)

# The torn transaction never committed: 400 + 5 rows in a, 1 in b.
if [ "$ic" = "ok " ] && [ "$cnt" = "405/1" ]; then
  echo "PASS: torn record-frame tail recovered and checkpointed cleanly ($cnt)"
else
  echo "FAIL: integrity_check='$ic' rows(a/b)='$cnt' (expected ok, 405/1)"
  exit 1
fi
