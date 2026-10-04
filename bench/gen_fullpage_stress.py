#!/usr/bin/env python3
"""Full-page (M2) differential workload (2026-10-02).

M2 replaces a clean mini-page that would grow to page size by an exact copy of
the leaf (BF_LOC_FULL), and the pager then fills a pcache miss on that leaf
from the copy instead of the WAL or the database file.  So the thing that can
go wrong is COHERENCE: a copy that outlives a change to its page serves stale
bytes to every later reader of that leaf -- point reads, scans, writes, and the
B-tree's own structure.

This workload makes copies on purpose and then changes their pages every way
SQLite can:
  * hot-leaf promotion: every row of a key range is read by rowid, repeatedly,
    so its leaf's mini-page grows until it is replaced by a full page (needs
    read promotion; run under the promo100 and ring variants too),
  * a page cache much smaller than the table (PRAGMA cache_size), so leaves are
    evicted from the pcache and come back through readDbPage,
  * then: UPDATE / INSERT / DELETE into those leaves, in committed, rolled-back
    and savepoint-rolled-back transactions; deletes that free pages; DROP and
    re-CREATE (freed pages reused by another table); DELETE FROM (truncate
    path); VACUUM (renumbering); checkpoints; and reads of the same ranges after
    every step.
The other engine is stock SQLite; outputs and the final .dump must match.

Usage: gen_fullpage_stress.py <seed> [n_rounds] [journal]
"""
import random
import sys


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: gen_fullpage_stress.py <seed> [n_rounds] [journal]\n")
        return 1
    seed = int(sys.argv[1])
    n_rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    journal = sys.argv[3] if len(sys.argv) > 3 else "wal"
    rng = random.Random(seed)
    out = []
    w = out.append

    n = rng.choice([3000, 6000])
    vlen = rng.choice([8, 20, 40])
    # page_size must precede journal_mode.  Small pages = more leaves per row
    # range, more full-page copies, and a VACUUM that renumbers more of them.
    w("PRAGMA page_size=%d;" % rng.choice([1024, 4096]))
    # No auto_vacuum axis: BF is off on auto-vacuum databases
    # (btreeUsesBfCache), so those runs would test nothing.
    w("PRAGMA journal_mode=%s;" % journal)
    # A small pcache: leaves must leave it and come back through readDbPage,
    # which is where a full page is served.  Same setting on both engines.
    w("PRAGMA cache_size=%d;" % rng.choice([20, 40, 80]))
    w("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);")
    w("CREATE TABLE u(id INTEGER PRIMARY KEY, v TEXT);")
    w("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<%d) "
      "INSERT INTO t SELECT i*2, printf('%%0%dd', i) FROM c;" % (n, vlen))
    w("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<%d) "
      "INSERT INTO u SELECT i, printf('%%0%dd', i*7) FROM c;" % (n // 2, vlen))
    if journal == "wal":
        w(".output /dev/null")
        w("PRAGMA wal_checkpoint(TRUNCATE);")
        w(".output stdout")

    def text():
        return "".join(rng.choice("abcdefghij") for _ in range(rng.choice([1, vlen, vlen + 30])))

    def hot_reads(tbl, lo, hi, reps):
        # Point read every row of [lo, hi] by rowid, `reps` times: OP_SeekRowid
        # per row, so each row is a promotion candidate; a leaf's mini-page
        # grows until the full-page trigger fires.
        for _ in range(reps):
            w("WITH RECURSIVE c(i) AS (SELECT %d UNION ALL SELECT i+1 FROM c WHERE i<%d) "
              "SELECT count(*), total(length((SELECT v FROM %s WHERE id=c.i))), "
              "sum(unicode((SELECT v FROM %s WHERE id=c.i))) FROM c;" % (lo, hi, tbl, tbl))

    def check(tbl, lo, hi):
        w("SELECT count(*), total(length(v)), min(id), max(id) FROM %s;" % tbl)
        w("SELECT group_concat(id||':'||v) FROM %s WHERE id BETWEEN %d AND %d;" % (tbl, lo, hi))
        w("SELECT id, v FROM %s WHERE id>=%d ORDER BY id LIMIT 5;" % (tbl, lo))
        w("SELECT id FROM %s WHERE id<=%d ORDER BY id DESC LIMIT 5;" % (tbl, hi))
        k = rng.randint(lo, hi)
        w("SELECT id, v FROM %s WHERE id=%d;" % (tbl, k))

    def mutate(tbl, lo, hi):
        r = rng.random()
        k = rng.randint(lo, hi)
        if r < 0.35:
            w("UPDATE %s SET v='%s' WHERE id=%d;" % (tbl, text(), k))
        elif r < 0.50:
            w("INSERT OR REPLACE INTO %s VALUES(%d,'%s');" % (tbl, k | 1, text()))
        elif r < 0.65:
            w("DELETE FROM %s WHERE id=%d;" % (tbl, k))
        elif r < 0.75:
            w("DELETE FROM %s WHERE id BETWEEN %d AND %d;" % (tbl, k, k + rng.randint(5, 200)))
        elif r < 0.85:
            w("UPDATE %s SET v=v||'%s' WHERE id BETWEEN %d AND %d;" % (tbl, text(), k, k + 30))
        else:
            # a burst of inserts into the hot range: splits the cached leaf
            for j in range(rng.randint(20, 120)):
                w("INSERT OR IGNORE INTO %s VALUES(%d,'%s');" % (tbl, k + j * 2 + 1, text()))

    for _ in range(n_rounds):
        tbl = rng.choice(["t", "t", "u"])
        top = 2 * n if tbl == "t" else n // 2
        lo = rng.randint(1, max(1, top - 400))
        hi = lo + rng.randint(50, 400)
        hot_reads(tbl, lo, hi, rng.randint(2, 6))
        check(tbl, lo, hi)
        r = rng.random()
        if r < 0.35:
            w("BEGIN;")
            for _ in range(rng.randint(1, 6)):
                mutate(tbl, lo, hi)
            if rng.random() < 0.5:
                hot_reads(tbl, lo, hi, 1)              # reads inside the txn
                check(tbl, lo, hi)
            w("COMMIT;")
        elif r < 0.55:
            w("BEGIN;")
            for _ in range(rng.randint(1, 6)):
                mutate(tbl, lo, hi)
            hot_reads(tbl, lo, hi, 1)
            check(tbl, lo, hi)
            w("ROLLBACK;")
        elif r < 0.65:
            w("BEGIN;")
            mutate(tbl, lo, hi)
            w("SAVEPOINT s1;")
            for _ in range(rng.randint(1, 4)):
                mutate(tbl, lo, hi)
            hot_reads(tbl, lo, hi, 1)
            w("ROLLBACK TO s1;")
            check(tbl, lo, hi)
            w("RELEASE s1;")
            w("COMMIT;" if rng.random() < 0.6 else "ROLLBACK;")
        elif r < 0.70:
            # page reuse: u's pages freed and taken by a new u of another shape
            w("DROP TABLE u;")
            w("CREATE TABLE u(id INTEGER PRIMARY KEY, v TEXT);")
            m = rng.choice([200, n // 2, n])
            w("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<%d) "
              "INSERT INTO u SELECT i, printf('%%0%dd', i*%d) FROM c;" % (m, vlen, rng.randint(2, 9)))
            n_u = m
            w("SELECT count(*), total(length(v)) FROM u;")
        elif r < 0.73:
            w("VACUUM;")
            check(tbl, lo, hi)
        elif r < 0.85 and journal == "wal":
            w(".output /dev/null")
            w("PRAGMA wal_checkpoint(%s);" % rng.choice(["PASSIVE", "TRUNCATE"]))
            w(".output stdout")
        else:
            for _ in range(rng.randint(1, 4)):
                mutate(tbl, lo, hi)                   # autocommit
        check(tbl, lo, hi)
        hot_reads(tbl, lo, hi, 1)

    w("SELECT count(*), total(length(v)) FROM t;")
    w("SELECT count(*), total(length(v)) FROM u;")
    w("PRAGMA integrity_check;")
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
