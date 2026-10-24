#!/bin/sh
# Deferred commit + a rollback that discards spilled frames (2026-10-24).
#
# With bf_deferred_commit=8, autocommit writes wait in the open group batch.  A
# later transaction spills its dirty pages (cache_size=20 and a long UPDATE),
# and the spill writes the staged batch -- COMMITTED records -- into WAL frames
# of that transaction.  Its ROLLBACK discards those frames.  The rebuild kept
# the records only in the cache, and the next read transaction's full rebuild
# dropped them: rows 413 and 131 lost their acknowledged writes.  Found by
# difftest group8 gen_fullpage_stress seed 312, delta-reduced to this script.
# Fix: the discarded ops join the group carry (rollback_discard_carried);
# ablation SQLITE_BF_NO_DISCARD_CARRY brings the bug back.
#
# Usage: sh bench/discard_carry_repro.sh [sqlite3_buf] [sqlite3_stock]
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$(realpath "${1:-$HERE/../build/sqlite3_buf}")
STOCK=$(realpath "${2:-$HERE/../build/sqlite3_stock}")
DIR=$(mktemp -d /tmp/bf_dcarry.XXXX)
trap 'rm -rf "$DIR"' EXIT

script() {
  cat <<'EOF'
PRAGMA page_size=4096;
PRAGMA journal_mode=wal;
PRAGMA cache_size=20; PRAGMA bf_group_commit=8;
CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);
CREATE TABLE u(id INTEGER PRIMARY KEY, v TEXT);
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<3000) INSERT INTO t SELECT i*2, printf('%040d', i) FROM c;
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<1500) INSERT INTO u SELECT i, printf('%040d', i*7) FROM c;
.output /dev/null
PRAGMA wal_checkpoint(TRUNCATE);
UPDATE t SET v='aeigcdceejfcbidciddegagjdecichacfccjiijdbcacceggddicaejhdabibchcedgicj' WHERE id=5254;
PRAGMA wal_checkpoint(PASSIVE);
.output stdout
INSERT OR REPLACE INTO t VALUES(413,'a');
INSERT OR REPLACE INTO u VALUES(131,'cfedgaedhggdhjfjhhbjajeigcgicfbcbiibddgggihhfhchbbidghgbgfeihgehhaggaa');
BEGIN;
INSERT OR IGNORE INTO t VALUES(3613,'fagchfifcfddgahaaaebfhhcabiccdfebaeeeeabdfifhjjbafjgicdjbijiahcdagfdhe');
SAVEPOINT s1;
UPDATE t SET v='afhfcbbifecfchhhhjabifgdjfhjiagbeidcahegeadjaheiehcjdeijgbdbcgfeibjaia' WHERE id=3516;
ROLLBACK;
SELECT id, v FROM u WHERE id=131;
SELECT id, v FROM t WHERE id IN (413,3516,3613,5254) ORDER BY id;
SELECT count(*), total(length(v)) FROM t;
SELECT count(*), total(length(v)) FROM u;
EOF
}

{ script; printf 'PRAGMA bf_cache_stats;\n'; } | "$BIN" "$DIR/bf.db" > "$DIR/bf.all" 2>&1
script | "$STOCK" "$DIR/st.db" > "$DIR/st.out" 2>&1
grep -vE '^[a-z_]+\|' "$DIR/bf.all" > "$DIR/bf.out"
n=$(grep '^rollback_discard_carried|' "$DIR/bf.all" | cut -d'|' -f2)
if ! cmp -s "$DIR/bf.out" "$DIR/st.out"; then
  echo "discard_carry: BUG -- acknowledged deferred commits lost by a ROLLBACK (carried=${n:-?})"
  diff "$DIR/bf.out" "$DIR/st.out" | head -10
  exit 1
fi
# Vacuity guard: the script must still reach the discarded-frames path.
if [ "${n:-0}" -eq 0 ]; then
  echo "discard_carry: VACUOUS -- rollback_discard_carried=${n:-?}; retune the script"
  exit 1
fi
echo "discard_carry: OK (rollback_discard_carried=$n)"
