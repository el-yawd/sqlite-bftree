#!/bin/sh
# Recovery into a ring smaller than the committed dirty set (2026-10-12).
#
# A replay cannot evict what it writes -- every replayed record is DIRTY -- so
# before this was fixed a crash-recovery (or rollback, or another connection's
# snapshot rebuild) into a ring too small for the buffered-but-unmerged records
# DROPPED them: 12,000 committed single-row UPDATEs, writer killed, reopened
# with bf_cache_size=262144 -> 9,661 survived (replay_dropped 2339).  The fix
# sizes the ring for the replay before it runs (sqlite3BfCacheReplayBound,
# counters ring_grow / ring_peak) and fails with SQLITE_FULL instead of
# answering from a cache that lost rows.
#
# Usage: sh bench/replay_fit_repro.sh [BUF] [STOCK]
set -e
cd "$(dirname "$0")"
BUF=${1:-../build/sqlite3_buf}
STOCK=${2:-../build/sqlite3_stock}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
python3 - > "$W/w.sql" <<'EOF'
import random
random.seed(5)
print("PRAGMA journal_mode=wal;\nPRAGMA wal_autocheckpoint=0;\nPRAGMA bf_cache_size=33554432;")
print("CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT);")
print("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<20000) "
      "INSERT INTO t SELECT x, printf('%.100c','a') FROM c;")
print("PRAGMA wal_checkpoint(TRUNCATE);")
keys = random.sample(range(1, 20001), 12000)
for i in range(0, len(keys), 500):
    print("BEGIN;")
    for k in keys[i:i+500]:
        print(f"UPDATE t SET v=printf('%.100c','b') WHERE k={k};")
    print("COMMIT;")
print(".system kill -9 $PPID")
EOF
"$BUF" "$W/t.db" < "$W/w.sql" >/dev/null 2>&1 || true
[ -s "$W/t.db-wal" ] || { echo "FAIL setup: no WAL left behind"; exit 1; }
# bf_cache_size BEFORE anything reads the file: recovery runs on the first read.
got=$(printf "PRAGMA bf_cache_size=262144;\nSELECT count(*), sum(v LIKE 'b%%') FROM t;\n" \
      | "$BUF" "$W/t.db" 2>&1 | tail -1)
want="20000|12000"
if [ "$got" = "$want" ]; then
  echo "OK   recovery into a 256 KiB ring kept every committed record ($got)"
else
  echo "FAIL recovery into a 256 KiB ring: got '$got', want '$want'"
  exit 1
fi
