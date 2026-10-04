#!/bin/sh
# M2 coherence across connections (2026-10-03): a full-page copy must not
# outlive another connection's write to its leaf.
#
# Connection "main" reads a key range until its leaves become full pages
# (BF_LOC_FULL), then a second pager on the SAME file (ATTACH ... AS b) updates
# those rows.  main's pager sees another writer and resets; the reset reaches
# bfCacheTruncate, which drops every full page (sqlite3BfFullPageDropFrom).
# Without that drop main serves its stale copies: measured on a mutant, 51 of
# 301 changed rows visible, and old values.
#
# Single-connection oracles cannot reach this (every difftest generator uses
# one connection), and a general second-connection generator cannot be used
# either: BF does not support two connections writing one file -- a record
# buffered in one is invisible to the other (plan S1) -- and that fails without
# M2 too.  So this is the narrow, deterministic case that passes today.
#
# Usage: sh bench/fullpage_reset_repro.sh [sqlite3_buf] [sqlite3_stock]
# Arguments are relative to the caller's directory; defaults to bench/.
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$(realpath "${1:-$HERE/../build/sqlite3_buf}")
STOCK=$(realpath "${2:-$HERE/../build/sqlite3_stock}")
cd "$HERE"
DIR=$(mktemp -d /tmp/bf_fpreset.XXXX)
trap 'rm -rf "$DIR"' EXIT

script() {   # $1 = db path
  cat <<EOF
PRAGMA journal_mode=wal;
PRAGMA cache_size=10;
PRAGMA bf_promotion_rate=100;
CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<4000) INSERT INTO t SELECT i, printf('%020d', i) FROM c;
ATTACH '$1' AS b;
WITH RECURSIVE c(i) AS (SELECT 100 UNION ALL SELECT i+1 FROM c WHERE i<400) SELECT count(*), total(length((SELECT v FROM t WHERE id=c.i))) FROM c;
WITH RECURSIVE c(i) AS (SELECT 100 UNION ALL SELECT i+1 FROM c WHERE i<400) SELECT count(*), total(length((SELECT v FROM t WHERE id=c.i))) FROM c;
WITH RECURSIVE c(i) AS (SELECT 100 UNION ALL SELECT i+1 FROM c WHERE i<400) SELECT count(*), total(length((SELECT v FROM t WHERE id=c.i))) FROM c;
UPDATE b.t SET v='CHANGED' WHERE id BETWEEN 100 AND 400;
WITH RECURSIVE c(i) AS (SELECT 3000 UNION ALL SELECT i+1 FROM c WHERE i<4000) SELECT count(*) FROM c WHERE (SELECT v FROM t WHERE id=c.i) IS NOT NULL;
SELECT count(*) FROM main.t WHERE v='CHANGED';
SELECT group_concat(v) FROM (SELECT v FROM main.t WHERE id BETWEEN 200 AND 205);
EOF
}

{ script "$DIR/bf.db"; printf '.output stdout\nPRAGMA bf_cache_stats;\n'; } \
  | "$BIN" "$DIR/bf.db" > "$DIR/bf.all" 2>&1
script "$DIR/st.db" | "$STOCK" "$DIR/st.db" > "$DIR/st.out" 2>&1
grep -vE '^[a-z_]+\|' "$DIR/bf.all" > "$DIR/bf.out"
# Vacuity guard: the test means nothing unless main served full pages.  (Not
# drops: a missing drop is exactly the bug, and must report as BUG.)
reads=$(grep '^full_page_reads|' "$DIR/bf.all" | cut -d'|' -f2)
drops=$(grep '^full_page_drops|' "$DIR/bf.all" | cut -d'|' -f2)
if [ "${reads:-0}" -eq 0 ]; then
  echo "fullpage_reset: VACUOUS -- full_page_reads=${reads:-?} drops=${drops:-?}; retune the script"
  exit 1
fi
if cmp -s "$DIR/bf.out" "$DIR/st.out"; then
  echo "fullpage_reset: OK (full_page_reads=$reads drops=$drops)"
  exit 0
fi
echo "fullpage_reset: BUG -- main served stale leaves after another connection wrote"
diff "$DIR/bf.out" "$DIR/st.out" | head -10
exit 1
