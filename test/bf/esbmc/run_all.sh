#!/bin/sh
# Run every ESBMC proof of the BF-Tree fork, with UNWINDING ASSERTIONS ON.
#
#   sh test/bf/esbmc/run_all.sh          (from anywhere; ~1-3 min, <6 GB)
#
# Unwinding assertions are the point: with --no-unwinding-assertions ESBMC
# silently drops every path that needs more loop iterations than --unwind,
# and "VERIFICATION SUCCESSFUL" can mean "nothing failed in the first 2
# iterations".  The first version of these harnesses (2026-10-24) passed that
# way at --unwind 2 over 8-iteration loops; with assertions on it FAILED.
# Every bound below is large enough for every loop, and ESBMC proves it.
#
# The rowid harness proves a COPY of three static functions (they live in
# bf_btree.c / bf_mini_page.c, which need all of SQLite to compile).  Step 1
# checks that each copy still equals src/ token for token, so the proof
# cannot silently drift from the code.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
cd "$HERE"

echo "== 1. harness copies match src/"
python3 - "$ROOT" <<'PY'
import re, sys
root = sys.argv[1]
def fn(text, sig):
    i = text.index(sig)
    j = text.index("\n}\n", i) + 2
    body = text[i:j]
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)   # comments
    return " ".join(body.split())                       # whitespace
h = open("harness_rowid_encoding.c").read()
src = {"bfEncodeRowid": "src/bf_btree.c", "bfDecodeRowid": "src/bf_btree.c",
       "bfKeyCompare": "src/bf_mini_page.c"}
sigs = {"bfEncodeRowid": "static void bfEncodeRowid(i64 rowid, u8 *pBuf){",
        "bfDecodeRowid": "static i64 bfDecodeRowid(const u8 *pKey){",
        "bfKeyCompare": "static int bfKeyCompare(const u8 *pKey1, int nKey1, const u8 *pKey2, int nKey2){"}
bad = 0
for name, path in src.items():
    a = fn(open(root + "/" + path).read(), sigs[name])
    b = fn(h, sigs[name])
    print("   %-14s %s" % (name, "identical" if a == b else "DIFFERS from " + path))
    bad += a != b
sys.exit(1 if bad else 0)
PY

run(){ # name, file, unwind, extra
  printf "== %s: " "$1"
  out=$( (ulimit -v 6000000; timeout 1800 esbmc "$2" --overflow-check --unwind "$3" $4) 2>&1 ) || true
  if echo "$out" | grep -q "VERIFICATION SUCCESSFUL"; then
    echo "PROVEN (unwind $3, unwinding assertions on)"
  else
    echo "NOT PROVEN"; echo "$out" | grep -E "Violated property" -A3 | head -8; FAIL=1
  fi
}
FAIL=0
run "2. rowid order + round-trip (all i64)"      harness_rowid_encoding.c      9
run "3. decoder memory safety + progress (48 B)" harness_wal_decoder_safety.c 50
run "4a. codec round-trip, 1 INSERT"            harness_wal_codec.c          70 -DOPFIX=0
run "4b. codec round-trip, 1 DELETE"            harness_wal_codec.c          70 -DOPFIX=1
run "4c. codec round-trip, 1 MERGED"            harness_wal_codec.c          70 -DOPFIX=3
run "4d. codec round-trip, 1 CKPT"              harness_wal_codec.c          70 -DOPFIX=4
run "5. codec round-trip, 2 records, row ops"    harness_wal_codec.c          70 -DROWOPS2
exit $FAIL
