#!/usr/bin/env python3
"""Generate a CROSS-TABLE write workload under ring pressure.

Why this exists, and why the other generators cannot replace it.

Every other generator in bench/ drives ONE table.  That hides a whole class of
bug, because the insert fallback path flushes the cursor's table --
sqlite3BfBtreeFlushTableForMutation -- on the first BF refusal.  With one table
that cleans everything, so the ring's live mini-pages are clean by the time the
FIFO sweep runs and several states are simply unreachable:

  * a mini-page belonging to table A sitting dirty at the ring head while the
    writer is inserting into table B;
  * a CLEAN mini-page being written into while the ring is full, which is what
    the upgrade/compaction evict-and-retry paths need in order to misbehave.

The second state is the one that mattered on 2026-09-21: evictCallback ignored
pEvictProtect, unlinked the very slab a caller was copying from (locType =
BF_LOC_NULL), and the caller then re-pointed pPage without restoring locType --
leaving a live record that every read skips.  sqlite3BfRecordWrite returned
BF_OK and sqlite3BfRecordRead returned BF_NOT_FOUND for the same key one
instruction later.

Note what this generator does and does not do.  It covers the cross-table
shape, which is a real structural gap.  It does NOT currently reproduce that
bug: the only demonstrated trigger was M1's stall drain flushing a leaf and
then buffering into it, and a build with the fix reverted passes every case
here.  Do not treat a green run as coverage of it.

So: many tables, interleaved, in ONE transaction, with a ring far too small to
hold the working set.  Deletes and updates are woven in so the merge path and
tombstones are exercised at the same time.

Usage: gen_xtable.py SEED [JOURNAL] [NTABLES] [ROWS_PER_TABLE]
"""
import random
import sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
journal = sys.argv[2] if len(sys.argv) > 2 else "wal"
ntab = int(sys.argv[3]) if len(sys.argv) > 3 else 12
rows = int(sys.argv[4]) if len(sys.argv) > 4 else 3000

rnd = random.Random(seed)

out = []
w = out.append
w("PRAGMA journal_mode=%s;" % journal)
# BF pragmas AFTER journal_mode -- journal_mode reopens the pager and drops
# anything configured before it.  See BF_TREE_V2_PLAN.md 3.4.
w("PRAGMA bf_cache_size=262144;")
for t in range(ntab):
    w("CREATE TABLE t%d(id INTEGER PRIMARY KEY, v TEXT);" % t)

# One transaction: the ring must fill while records are still unflushed.
w("BEGIN;")
val = "x" * 100
for i in range(1, rows + 1):
    for t in range(ntab):
        w("INSERT INTO t%d VALUES(%d,'%s');" % (t, i, val))

# Interleave deletes and updates across a different subset of tables, so a
# table's dirty mini-pages are at the ring head while another table is written.
for i in range(1, rows + 1, 7):
    for t in range(0, ntab, 3):
        w("DELETE FROM t%d WHERE id=%d;" % (t, i))
        if t + 1 < ntab:
            w("UPDATE t%d SET v='y' WHERE id=%d;" % (t + 1, i))
w("COMMIT;")

# READ phase, then WRITE the rows just read: an ATTEMPT to make a clean
# mini-page (one holding only promoted BFOP_CACHE records) receive its first
# dirty record while the ring is full.  It did not succeed in reproducing the
# pEvictProtect bug -- kept because the read-then-write mix is worth covering
# on its own, not because it closes that hole.
w("PRAGMA bf_promotion_rate=100;")
for _ in range(rows):
    t = rnd.randrange(ntab)
    i = rnd.randint(1, rows)
    w("SELECT v FROM t%d WHERE id=%d;" % (t, i))

w("BEGIN;")
for _ in range(rows):
    t = rnd.randrange(ntab)
    i = rnd.randint(1, rows)
    w("INSERT OR REPLACE INTO t%d VALUES(%d,'w%d');" % (t, i, i))
w("COMMIT;")

# A further transaction that re-touches rows the earlier ones buffered, so reads
# go through the merge path against a ring that has already cycled.
w("BEGIN;")
for _ in range(rows // 4):
    t = rnd.randrange(ntab)
    i = rnd.randint(1, rows)
    if rnd.random() < 0.5:
        w("INSERT OR REPLACE INTO t%d VALUES(%d,'z%d');" % (t, i, i))
    else:
        w("DELETE FROM t%d WHERE id=%d;" % (t, i))
w("COMMIT;")

# Verification the differential compares byte for byte.
w("PRAGMA integrity_check;")
for t in range(ntab):
    w("SELECT %d, count(*), sum(length(v)), min(id), max(id) FROM t%d;" % (t, t))
    w("SELECT id, v FROM t%d ORDER BY id LIMIT 5;" % t)

print("\n".join(out))
