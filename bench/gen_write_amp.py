#!/usr/bin/env python3
"""Write-amplification workload for the Phase-2 claim:
"a commit persists a small record, not a 4 KB page".

Two phases:
  1. SEED   - bulk-load a committed base tree, then checkpoint(TRUNCATE) so the
              WAL is empty and every seeded row is a real base cell.  Stats and
              WAL size are sampled here, so the measured numbers are DELTAS and
              the seed's own buffering does not pollute them.
  2. MEASURE- n_txn transactions of rows_per_txn brand-new rowids scattered
              across the whole key space (so each transaction touches many
              distinct leaves), then sample again.

Stock SQLite must write one 4 KB page image per touched leaf per commit; the BF
build should write only record frames plus the single page-1 frame the WAL
format requires per commit.

Usage: gen_write_amp.py <db_path> <seed_rows> <n_txn> <rows_per_txn> [rng_seed]
"""
import random
import sys


def main():
    if len(sys.argv) < 5:
        sys.stderr.write(__doc__)
        return 1
    db = sys.argv[1]
    seed_rows = int(sys.argv[2])
    n_txn = int(sys.argv[3])
    per = int(sys.argv[4])
    rng = random.Random(int(sys.argv[5]) if len(sys.argv) > 5 else 7)
    w = print

    w("PRAGMA journal_mode=wal;")
    w("PRAGMA synchronous=NORMAL;")
    w("PRAGMA wal_autocheckpoint=0;")
    # Group commit size, if requested (stock ignores the unknown pragma).
    import os
    if os.environ.get("BF_GROUP"):
        w("PRAGMA bf_group_commit=%d;" % int(os.environ["BF_GROUP"]))
    w("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);")
    # Seed: rowids spaced 1000 apart so the measured inserts fall in the gaps.
    w("INSERT INTO t SELECT value*1000, hex(randomblob(32)) "
      "FROM generate_series(1,%d);" % seed_rows)
    w("PRAGMA wal_checkpoint(TRUNCATE);")
    w("SELECT 'MARK-BEGIN';")
    w("PRAGMA bf_cache_stats;")
    w(".shell wc -c < %s-wal" % db)

    used = set()
    for _ in range(n_txn):
        if per > 1:
            w("BEGIN;")
        k = 0
        while k < per:
            rid = rng.randrange(1, seed_rows) * 1000 + rng.randrange(1, 999)
            if rid in used:
                continue
            used.add(rid)
            k += 1
            w("INSERT INTO t VALUES(%d, hex(randomblob(32)));" % rid)
        if per > 1:
            w("COMMIT;")

    w("SELECT 'MARK-END';")
    w("PRAGMA bf_cache_stats;")
    w(".shell wc -c < %s-wal" % db)
    return 0


if __name__ == "__main__":
    sys.exit(main())
