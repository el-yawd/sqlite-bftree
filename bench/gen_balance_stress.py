#!/usr/bin/env python3
"""Dirty-sibling balance differential workload (2026-10-17).

A single-leaf merge (merge-on-full) applies a leaf's buffered records through a
cursor; when that splits or rebalances the leaf, balance_nonroot can reach
SIBLINGS that hold dirty records of their own, whose keys may then belong to
another leaf.  bfEscalateIfMisplaced repairs that by merging those siblings
(the ones sqlite3BfBtreeForgetPage listed), falling back to the whole table.
No other generator reaches it -- every one of them reports dirty_balance 0 --
because it needs full base leaves AND full mini-pages on neighbouring leaves at
the same time: 2,000 rows of 90 B in base pages (checkpointed), then inserts of
the keys in between, with updates and deletes, in 25-statement transactions,
and reads every 400 statements.

Usage: gen_balance_stress.py <seed> [n_ops]
"""
import random
import sys


def main():
    seed = int(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    r = random.Random(seed)
    w = print
    w("CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT);")
    w("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<2000) "
      "INSERT INTO t SELECT x*4, printf('%.90c','a') FROM c;")
    w("PRAGMA wal_checkpoint(TRUNCATE);")
    keys = [k for k in range(1, 8001) if k % 4]
    r.shuffle(keys)
    for i, k in enumerate(keys[:n]):
        if i % 25 == 0:
            w("BEGIN;")
        op = r.random()
        if op < 0.8:
            w("INSERT INTO t VALUES(%d, printf('%%.%dc','b'));" % (k, r.randint(60, 120)))
        elif op < 0.9:
            w("UPDATE t SET v=printf('%%.%dc','c') WHERE k=%d;"
              % (r.randint(60, 120), r.choice(range(4, 8001, 4))))
        else:
            w("DELETE FROM t WHERE k=%d;" % r.choice(range(4, 8001, 4)))
        if i % 25 == 24:
            w("COMMIT;")
        if i % 400 == 399:
            w("SELECT count(*), total(length(v)) FROM t;")
            w("SELECT k, length(v) FROM t WHERE k BETWEEN %d AND %d;" % (k - 6, k + 6))
            w("SELECT k FROM t WHERE k IN (%d,%d,%d);" % (k, k + 1, k + 2))
    if min(n, len(keys)) % 25:
        w("COMMIT;")
    w("SELECT count(*), total(length(v)), total(k) FROM t;")


if __name__ == "__main__":
    main()
