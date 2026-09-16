#!/bin/sh
# Reproducer: the circular buffer's free list is corrupted by the FIFO eviction
# head, segfaulting in bfFreeListRemove.
#
# The free list threads its "next" pointer through the freed blocks themselves.
# Nothing removes a block from the list when the eviction head sweeps past it,
# so once the tail wraps and reuses that address, the first 8 bytes are payload
# and following the chain walks into it:
#
#   #0 bfFreeListRemove (pFl->apHead[idx] = *ppNext)
#   #1 sqlite3BfCircularBufferAlloc
#   #2 sqlite3BfRecordWrite
#   #3 sqlite3BfBtreePromoteRecord   <- an ordinary read promoting a record
#
# It needs the ring to actually CYCLE, which is why it stayed hidden: every read
# benchmark in this repo reports evictions=0, and the differential oracles use
# databases far too small to fill the ring.  Give the ring a working set it
# cannot hold and it crashes within seconds.
#
# Usage: sh bench/ring_repro.sh [path-to-bfbench]   (default: harness bf SUT)
set -e
cd "$(dirname "$0")/harness"
BIN="${1:-./bin/bfbench_bf}"
DB=$(mktemp -u "${TMPDIR:-/tmp}/bf_ring_repro.XXXX.db")
trap 'rm -f "$DB" "$DB-wal" "$DB-shm"' EXIT

[ -f work/datasets/v100.db ] || { echo "needs work/datasets/v100.db (runner.py builds it)"; exit 2; }
cp work/datasets/v100.db "$DB"; chmod u+w "$DB"

# 4M rows against a 32 MiB ring, promoting every miss: the ring cycles hard.
if "$BIN" run --db "$DB" --records 4000000 --value-len 100 --key-spacing 16 \
     --workload read=100 --dist zipf --theta 0.99 --seconds 10 --warmup-seconds 20 \
     --bf-cache-bytes 33554432 --page-cache-bytes 16777216 --promotion 100 \
     --read-txn --sut bf --label ring_repro --json /dev/null >/dev/null 2>&1
then
  echo "OK   ring cycled without crashing"
else
  rc=$?
  [ $rc -eq 139 ] && echo "BUG  segfault (exit 139) in the free list" || echo "BUG  exit=$rc"
  exit 1
fi
