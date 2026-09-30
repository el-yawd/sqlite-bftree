#!/usr/bin/env python3
"""Blind-insert differential workload (D3b, 2026-09-29).

D3b skips OP_NotExists's leaf read for an INSERT OR REPLACE into a rowid table
with no index, trigger or foreign key, and buffers an upsert against the child
page -- but only when a bitmap says that child is a known LEAF.  So the thing
that can go wrong is the bitmap: a page that was a leaf and is now something
else (freed and reused as an interior or overflow page, un-allocated by a
rollback, renumbered by VACUUM, truncated away).  A wrong bit buffers a record
against a page that is not a leaf, which no read path will ever find.

This workload churns page identities as hard as it can while doing blind
upserts into the same tables:
  * INSERT OR REPLACE of new and existing keys (gaps, appends, overwrites of
    base rows, of buffered rows, of deleted rows), with overflow-sized values
    (the refused path) and tiny ones,
  * splits then ROLLBACK / ROLLBACK TO (pages allocated then un-allocated),
  * DROP TABLE + CREATE TABLE (freed pages reused by another table, possibly as
    interior pages), DELETE FROM t (the truncate path), VACUUM, checkpoints,
  * a table that starts as a single root leaf and grows under blind upserts,
  * a control table WITH an index (never blind) and auto-rowid inserts after
    blind upserts of large keys (OP_NewRowid must see the buffered max rowid),
with reads after every transaction.  Output and final .dump are compared.

Usage: gen_blind_stress.py <seed> [n_txns] [journal]
"""
import os
import random
import sys


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: gen_blind_stress.py <seed> [n_txns] [journal]\n")
        return 1
    seed = int(sys.argv[1])
    n_txns = int(sys.argv[2]) if len(sys.argv) > 2 else 250
    journal = sys.argv[3] if len(sys.argv) > 3 else "wal"
    rng = random.Random(seed)
    skip = set(filter(None, os.environ.get("BLIND_SKIP", "").split(",")))
    out = []
    w = out.append

    def text(n=None):
        n = rng.choice([1, 6, 20, 40, 100]) if n is None else n
        return "".join(rng.choice("abcdefghijklmnop") for _ in range(n))

    def val():
        r = rng.random()
        if r < 0.04:
            return "'%s'||hex(zeroblob(%d))" % (text(4), rng.choice([300, 900, 3000]))
        return "'%s'" % text()

    # Small pages make DEEP trees at a few thousand rows.  That is the point: a
    # stale known-leaf bit is only observable when a page that was once a leaf
    # comes back as an INTERIOR page that a descent reaches as a child, which
    # needs depth >= 3.  At 4 KiB pages these tables stay at depth 2, every
    # child of the root is a leaf, and a generator with all bit-clearing
    # compiled out still passed 216/216 (2026-09-30).  So this generator emits
    # its own header: page_size must precede journal_mode.
    page = rng.choice([512, 512, 1024, 4096])
    w("PRAGMA page_size=%d;" % page)
    w("PRAGMA journal_mode=%s;" % journal)
    tables = {"a": 4000, "b": 1500, "g": 50}      # name -> key range
    w("CREATE TABLE a(id INTEGER PRIMARY KEY, v TEXT);")
    w("CREATE TABLE b(id INTEGER PRIMARY KEY, v TEXT);")
    w("CREATE TABLE g(id INTEGER PRIMARY KEY, v TEXT);")      # starts as a root leaf
    w("CREATE TABLE x(id INTEGER PRIMARY KEY, k INT, v TEXT);")
    w("CREATE INDEX xk ON x(k);")                              # never blind
    w("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<1200) "
      "INSERT INTO a SELECT i*3, printf('%012d', i) FROM c;")
    w("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<400) "
      "INSERT INTO b SELECT i*2, printf('%012d', i) FROM c;")
    if journal == "wal":
        w(".output /dev/null")
        w("PRAGMA wal_checkpoint(TRUNCATE);")
        w(".output stdout")
    w("SELECT count(*) FROM a; SELECT count(*) FROM b;")    # descents mark the leaves

    # Auto rowids after blind upserts of LARGE keys: OP_NewRowid must see the
    # buffered max.  Done before any rollback, because BF's buffered max-rowid
    # is monotonic by design (bf_cache.h) and a rolled-back insert would make
    # the auto-assigned rowids differ from stock without either being wrong.
    for t in ("a", "b"):
        for _ in range(rng.randint(3, 12)):
            w("INSERT OR REPLACE INTO %s VALUES(%d,%s);"
              % (t, tables[t] + rng.randint(1, 5000), val()))
        for _ in range(rng.randint(2, 6)):
            w("INSERT INTO %s(v) VALUES(%s);" % (t, val()))
        w("SELECT max(id), count(*) FROM %s;" % t)

    def key(t):
        n = tables[t]
        r = rng.random()
        if r < 0.15:
            return n + rng.randint(1, 50)          # append-ish
        return rng.randint(1, n)

    def write_op():
        t = rng.choice(["a", "a", "b", "g"])
        r = rng.random()
        if r < 0.55:
            w("INSERT OR REPLACE INTO %s VALUES(%d,%s);" % (t, key(t), val()))
        elif r < 0.62:
            w("REPLACE INTO %s(id,v) VALUES(%d,%s);" % (t, key(t), val()))
        elif r < 0.74:
            w("DELETE FROM %s WHERE id=%d;" % (t, key(t)))
        elif r < 0.78:
            lo = key(t)
            w("DELETE FROM %s WHERE id BETWEEN %d AND %d;" % (t, lo, lo + rng.randint(0, 60)))
        elif r < 0.84:
            w("UPDATE %s SET v=%s WHERE id=%d;" % (t, val(), key(t)))
        elif r < 0.90:
            w("INSERT OR REPLACE INTO x VALUES(%d,%d,%s);"
              % (rng.randint(1, 900), rng.randint(0, 30), val()))
        elif r < 0.94:
            # a burst of blind upserts into one gap: splits the leaf in-txn
            base = rng.randint(1, tables[t])
            for i in range(rng.randint(10, 60)):
                w("INSERT OR REPLACE INTO %s VALUES(%d,%s);" % (t, base + i * 0 + rng.randint(0, 3), val()))
        else:
            w("INSERT OR IGNORE INTO %s VALUES(%d,%s);" % (t, key(t), val()))

    def reads():
        for t in ("a", "b", "g"):
            w("SELECT count(*), total(length(v)), max(id) FROM %s;" % t)
            k = key(t)
            w("SELECT id, v FROM %s WHERE id=%d;" % (t, k))
            w("SELECT id FROM %s WHERE id>=%d ORDER BY id LIMIT 5;" % (t, k))
            w("SELECT id FROM %s WHERE id<=%d ORDER BY id DESC LIMIT 5;" % (t, k))
        w("SELECT count(*), total(k) FROM x;")
        w("SELECT id, v FROM x WHERE k=%d ORDER BY id;" % rng.randint(0, 30))

    for _ in range(n_txns):
        r = rng.random()
        if r < 0.40:
            w("BEGIN;")
            for _ in range(rng.randint(1, 10)):
                write_op()
            if rng.random() < 0.3:
                reads()
            w("COMMIT;")
        elif r < 0.55:
            w("BEGIN;")
            for _ in range(rng.randint(1, 10)):
                write_op()
            if rng.random() < 0.5:
                reads()
            w("ROLLBACK;")
        elif r < 0.63:
            w("BEGIN;")
            write_op()
            w("SAVEPOINT s1;")
            for _ in range(rng.randint(1, 8)):
                write_op()
            w("ROLLBACK TO s1;")
            write_op()
            w("RELEASE s1;")
            w("COMMIT;" if rng.random() < 0.6 else "ROLLBACK;")
        elif r < 0.66 and "drop" not in skip:
            # page reuse across tables: b's pages go to the freelist and come back
            # as whatever the new b needs (roots, interior pages, leaves)
            w("DROP TABLE b;")
            w("CREATE TABLE b(id INTEGER PRIMARY KEY, v TEXT);")
            n = rng.choice([30, 600, 2500])
            w("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<%d) "
              "INSERT INTO b SELECT i*2, printf('%%012d', i) FROM c;" % n)
            tables["b"] = n * 2
            w("SELECT count(*) FROM b;")
        elif r < 0.68 and "trunc" not in skip:
            w("DELETE FROM g;")                        # truncate optimisation
        elif r < 0.70 and "vacuum" not in skip:
            w("VACUUM;")                               # renumbers every page
            w("SELECT count(*) FROM a;")
        elif r < 0.76 and journal == "wal":
            w(".output /dev/null")
            w("PRAGMA wal_checkpoint(%s);" % rng.choice(["PASSIVE", "TRUNCATE"]))
            w(".output stdout")
        else:
            for _ in range(rng.randint(1, 4)):
                write_op()
        reads()

    w("PRAGMA integrity_check;")
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
