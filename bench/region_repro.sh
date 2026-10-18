#!/bin/sh
# C4b (2026-10-18): the mini-page region, end to end, deterministic.
#
# A checkpoint writes buffered records into the database file's region instead
# of merging them into their leaves.  Checks, against stock SQLite on the same
# statements (no region, every row in a leaf):
#   1. the rows survive a TRUNCATE checkpoint and a reopen (seeded from the
#      region), with a secondary index, a buffered DELETE and UPDATE;
#   2. stock SQLite refuses the file while a region exists (version 3);
#   3. PRAGMA integrity_check passes (it merges everything first);
#   4. DROP TABLE and VACUUM keep every row;
#   5. leaving WAL mode merges everything, empties the region, deletes the log,
#      and stock then reads the same rows.
#
# Usage: sh bench/region_repro.sh [sqlite3_buf] [sqlite3_stock]
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$(realpath "${1:-$HERE/../build/sqlite3_buf}")
STOCK=$(realpath "${2:-$HERE/../build/sqlite3_stock}")
DIR=$(mktemp -d /tmp/bf_region.XXXX)
trap 'rm -rf "$DIR"' EXIT
fail(){ echo "FAIL: $*"; exit 1; }

load() { cat <<'SQL'
PRAGMA journal_mode=wal;
PRAGMA wal_autocheckpoint=0;
CREATE TABLE a(k INTEGER PRIMARY KEY, v);
CREATE TABLE t(k INTEGER PRIMARY KEY, v, w);
CREATE INDEX tw ON t(w);
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<3000)
  INSERT INTO t SELECT x*3, printf('%020d', x), x FROM c;
INSERT INTO a VALUES(1, 'drop me');
PRAGMA wal_checkpoint(TRUNCATE);
INSERT INTO t VALUES(1, 'buffered one', -1);
INSERT INTO t VALUES(2, 'buffered two', -2);
UPDATE t SET v='updated' WHERE k=9;
DELETE FROM t WHERE k=12;
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<300)
  INSERT OR REPLACE INTO t SELECT x*31+1, 'b'||x, x+100000 FROM c;
PRAGMA wal_checkpoint(TRUNCATE);
SQL
}
Q="SELECT count(*), total(w), total(length(v)), (SELECT v FROM t WHERE k=9), (SELECT count(*) FROM t WHERE k=12) FROM t;"

load | "$BIN" "$DIR/bf.db" > /dev/null || fail "BF load"
load | "$STOCK" "$DIR/st.db" > /dev/null || fail "stock load"
want=$("$STOCK" "$DIR/st.db" "$Q")

got=$("$BIN" "$DIR/bf.db" "$Q")
[ "$got" = "$want" ] || fail "after reopen: '$got' want '$want'"
pages=$("$BIN" "$DIR/bf.db" "SELECT count(*) FROM t; PRAGMA bf_cache_stats" | sed -n "s/^region_seeds|//p")
[ "${pages:-0}" -ge 1 ] || fail "the reopen did not seed from a region"

"$STOCK" "$DIR/bf.db" "SELECT 1" > /dev/null 2>&1 && fail "stock opened a database with a region"

ic=$("$BIN" "$DIR/bf.db" "PRAGMA integrity_check")
[ "$ic" = "ok" ] || fail "integrity_check: $ic"

"$BIN" "$DIR/bf.db" "DROP TABLE a; VACUUM;" > /dev/null || fail "VACUUM"
"$STOCK" "$DIR/st.db" "DROP TABLE a; VACUUM;" > /dev/null
got=$("$BIN" "$DIR/bf.db" "$Q")
[ "$got" = "$want" ] || fail "after DROP TABLE + VACUUM: '$got' want '$want'"

# more buffered rows, a region again, then out of WAL mode
"$BIN" "$DIR/bf.db" "PRAGMA journal_mode=wal; INSERT INTO t VALUES(4,'late',-4); PRAGMA wal_checkpoint(TRUNCATE);" > /dev/null
"$STOCK" "$DIR/st.db" "INSERT INTO t VALUES(4,'late',-4);" > /dev/null
want=$("$STOCK" "$DIR/st.db" "$Q")
"$BIN" "$DIR/bf.db" "PRAGMA journal_mode=delete" > /dev/null || fail "leaving WAL mode"
[ -e "$DIR/bf.db-wal" ] && fail "the log survived leaving WAL mode"
got=$("$STOCK" "$DIR/bf.db" "$Q") || fail "stock cannot read the database after leaving WAL"
[ "$got" = "$want" ] || fail "stock after leaving WAL: '$got' want '$want'"
ic=$("$STOCK" "$DIR/bf.db" "PRAGMA integrity_check")
[ "$ic" = "ok" ] || fail "stock integrity_check: $ic"
echo "PASS: region round trip, refusal, integrity, VACUUM, leave-WAL ($got)"
