#!/usr/bin/env python3
"""Read hit-rate workload — measures what mini-page compaction costs the READ
cache.

Compaction (sqlite3BfRecordWrite) makes room for a dirty record by dropping a
mini-page's clean BFOP_CACHE / BFOP_PHANTOM records.  Those clean records ARE
the read cache, so a write-heavy phase can evict the working set that a
following read phase depends on.  This workload interleaves the two:

  1. SEED   - bulk load, checkpoint(TRUNCATE), sample stats (the mark).
  2. WARM   - Zipf-ish skewed point reads over the hot region, so the hot rows
              get promoted into mini-pages as BFOP_CACHE records.
  3. WRITE  - inserts scattered over the SAME leaves, which is what triggers
              compaction and drops those cached records.
  4. READ   - repeat the skewed point reads and sample stats again.

The phase-4 hit/miss delta is the number to compare between a build with
compaction and one built with -DSQLITE_BF_NO_MINIPAGE_COMPACT.

Usage: gen_read_hit.py <seed_rows> <n_reads> <n_writes> [rng_seed]
"""
import random
import sys


def main():
    if len(sys.argv) < 4:
        sys.stderr.write(__doc__)
        return 1
    seed_rows = int(sys.argv[1])
    n_reads = int(sys.argv[2])
    n_writes = int(sys.argv[3])
    rng = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else 7)
    w = print

    # The hot region is the first 10% of the key space; reads concentrate there.
    hot = max(1, seed_rows // 10)

    w("PRAGMA journal_mode=wal;")
    w("PRAGMA synchronous=NORMAL;")
    w("PRAGMA wal_autocheckpoint=0;")
    w("PRAGMA bf_promotion_rate=100;")   # promote every read, so the cache fills
    w("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);")
    w("INSERT INTO t SELECT value*1000, hex(randomblob(32)) "
      "FROM generate_series(1,%d);" % seed_rows)
    w("PRAGMA wal_checkpoint(TRUNCATE);")

    def reads(n):
        for _ in range(n):
            w("SELECT v FROM t WHERE id=%d;" % (rng.randrange(1, hot) * 1000))

    reads(n_reads)                                    # warm
    w("SELECT 'MARK-BEGIN';")
    w("PRAGMA bf_cache_stats;")

    used = set()
    for _ in range(n_writes):                         # write pressure, same leaves
        while True:
            rid = rng.randrange(1, hot) * 1000 + rng.randrange(1, 999)
            if rid not in used:
                break
        used.add(rid)
        w("INSERT INTO t VALUES(%d, hex(randomblob(32)));" % rid)

    w("SELECT 'MARK-READS';")
    w("PRAGMA bf_cache_stats;")
    reads(n_reads)                                    # the measured read phase
    w("SELECT 'MARK-END';")
    w("PRAGMA bf_cache_stats;")
    return 0


if __name__ == "__main__":
    sys.exit(main())
