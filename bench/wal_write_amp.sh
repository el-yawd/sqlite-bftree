#!/bin/sh
# Phase 2 write-amplification measurement: "a commit persists a small record,
# not a 4 KB page" (see gen_write_amp.py for the workload).
#
# Reports, for the MEASURED phase only (deltas across a checkpoint(TRUNCATE)
# mark), how many WAL bytes each build wrote, and for the BF build how those
# bytes split between record frames and page images:
#
#   wal_record_frames  small physiological record frames (the row data)
#   wal_page_frames    4 KB page images written anyway
#   wal_commits        commit frames
#   buffered_inserts   inserts absorbed by a mini-page (no base write)
#   insert_fallbacks   inserts that took the base-page write path
#
# The claim holds when page_frames/commits ~ 1 (the page-1 commit frame the WAL
# format requires) and insert_fallbacks stays near zero.
#
# NOTE: payloads must not be zeroblob() — a record with nZero>0 is never
# buffered by the insert hook, so it would silently take the base path.
#
# Usage: sh bench/wal_write_amp.sh [seed_rows] [n_txn] [rows_per_txn]
set -e
cd "$(dirname "$0")"
BUF=${BUF:-../build/sqlite3_buf}
STOCK=${STOCK:-../build/sqlite3_stock}
SEED_ROWS=${1:-5000}
NTXN=${2:-200}
PER=${3:-1}
WORK=./stress_work
mkdir -p "$WORK"

run() { # $1=binary $2=tag
  db="$WORK/wamp_$2.db"
  rm -f "$db" "$db-wal" "$db-shm"
  python3 ./gen_write_amp.py "$db" "$SEED_ROWS" "$NTXN" "$PER" > "$WORK/wamp_$2.sql"
  "$1" "$db" < "$WORK/wamp_$2.sql" > "$WORK/wamp_$2.out" 2>&1
}

report() { # $1=tag $2=label
  awk -v label="$2" -v ntxn="$NTXN" -v per="$PER" -F'|' '
    /^MARK-BEGIN$/ {phase=1; next}
    /^MARK-END$/   {phase=2; next}
    phase==1 && NF==2 {a[$1]=$2}
    phase==2 && NF==2 {b[$1]=$2}
    phase==1 && NF==1 && $1 ~ /^[0-9]+$/ {walA=$1}
    phase==2 && NF==1 && $1 ~ /^[0-9]+$/ {walB=$1}
    END{
      bytes = walB - walA
      printf "%-6s  wal_bytes=%-10d  %.0f bytes/commit\n", label, bytes, bytes/ntxn
      if( "wal_commits" in b ){
        c  = b["wal_commits"]      - a["wal_commits"]
        pg = b["wal_page_frames"]  - a["wal_page_frames"]
        rc = b["wal_record_frames"]- a["wal_record_frames"]
        bi = b["buffered_inserts"] - a["buffered_inserts"]
        fb = b["insert_fallbacks"] - a["insert_fallbacks"]
        mg = b["merges"]           - a["merges"]
        printf "        commits=%d  page_frames=%d (%.2f/commit)  record_frames=%d (%.2f/commit)\n", \
               c, pg, (c?pg/c:0), rc, (c?rc/c:0)
        printf "        buffered_inserts=%d  insert_fallbacks=%d  minipage_merges=%d\n", bi, fb, mg
      }
      printf "%s %d\n", "BYTES", bytes > "/dev/stderr"
    }
  ' "$WORK/wamp_$1.out"
}

echo "workload: seed=$SEED_ROWS rows, then $NTXN transactions x $PER row(s), WAL mode, autocheckpoint off"
echo ""
run "$STOCK" stock
run "$BUF"   buf
sb=$(report stock "stock" 2>&1 >/dev/null | awk '{print $2}')
report stock "stock"
bb=$(report buf "bf" 2>&1 >/dev/null | awk '{print $2}')
report buf   "bf"
echo ""
awk -v s="$sb" -v b="$bb" 'BEGIN{ if(b>0) printf "WAL bytes written: stock/bf = %.2fx\n", s/b }'
