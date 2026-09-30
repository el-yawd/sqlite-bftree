#!/usr/bin/env python3
"""Existing-row UPDATE differential workload (D3, 2026-09-29).

D3a buffers an UPDATE of a row that has a BASE cell as a BFOP_INSERT that
shadows it (D3-core).  The other generators rarely reach that state: their
rows are mostly still buffered (never checkpointed into base), so their UPDATEs
rewrite a mini-page record, not a base cell.  This one checkpoints a populated
base first and then updates it hard, so every read path has to prefer the
buffered value over the base cell of the same key:

  * point reads (descent shortcut, and single-leaf tables that have no descent),
    range scans, reverse scans, count, and scans after mid-session checkpoints,
  * same-size and size-changing updates, updates to and from overflow values
    (too big for a mini-page: the base-write fallback), repeated updates of
    one row, UPDATE ... SET v=v||... that reads the shadowed value,
  * a secondary-indexed table whose UPDATEs change the indexed column (the
    index entry is built from the OLD row, read through the table cursor) and
    index-driven multi-row UPDATEs (BTREE_SAVEPOSITION: not buffered),
  * delete / re-insert / update of shadowed rows, REPLACE, rowid-changing
    UPDATEs,
  * ROLLBACK, ROLLBACK TO a savepoint, and checkpoints between transactions,
with reads after every transaction.  Both engines run the same script; the
oracle compares outputs and the final .dump.

Usage: gen_update_stress.py <seed> [n_txns] [journal]
"""
import random
import sys


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: gen_update_stress.py <seed> [n_txns] [journal]\n")
        return 1
    seed = int(sys.argv[1])
    n_txns = int(sys.argv[2]) if len(sys.argv) > 2 else 250
    journal = sys.argv[3] if len(sys.argv) > 3 else "wal"
    rng = random.Random(seed)
    out = []
    w = out.append
    NT = rng.choice([300, 1500, 4000])       # rows in t (several leaves)
    NS = rng.choice([3, 12])                 # rows in s (one leaf: no descent)
    NU = 1200

    def text(n=None):
        n = rng.choice([0, 4, 11, 12, 30, 90]) if n is None else n
        return "".join(rng.choice("abcdefghijklmnop") for _ in range(n))

    def val():
        r = rng.random()
        if r < 0.05:     # overflow: too big for a mini-page, forces a base write
            return "'%s'||hex(zeroblob(%d))" % (text(6), rng.choice([700, 2600]))
        if r < 0.35:     # same length as the base rows: in-place both ways
            return "'%s'" % text(12)
        return "'%s'" % text()

    w("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);")
    w("CREATE TABLE s(id INTEGER PRIMARY KEY, v TEXT);")
    w("CREATE TABLE u(id INTEGER PRIMARY KEY, a INT, b TEXT);")
    w("CREATE INDEX ua ON u(a);")
    w("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<%d) "
      "INSERT INTO t SELECT i*3, printf('%%012d', i) FROM c;" % NT)
    w("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<%d) "
      "INSERT INTO s SELECT i, printf('%%012d', i) FROM c;" % NS)
    w("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<%d) "
      "INSERT INTO u SELECT i*2, i%%50, printf('%%012d', i) FROM c;" % NU)
    if journal == "wal":
        # Put the rows into BASE pages: from here on an UPDATE shadows a base
        # cell instead of rewriting a buffered record.
        w(".output /dev/null")
        w("PRAGMA wal_checkpoint(TRUNCATE);")
        w(".output stdout")

    def tid():
        if rng.random() < 0.85:
            return rng.randint(1, NT) * 3           # an existing base row
        return rng.randint(1, NT * 3)               # maybe absent / a gap

    hot = [tid() for _ in range(6)]                 # rows updated over and over

    def write_op():
        r = rng.random()
        if r < 0.30:
            w("UPDATE t SET v=%s WHERE id=%d;" % (val(), tid()))
        elif r < 0.40:
            w("UPDATE t SET v=%s WHERE id=%d;" % (val(), rng.choice(hot)))
        elif r < 0.46:
            w("UPDATE t SET v=v||'%s' WHERE id=%d;" % (text(3), rng.choice(hot)))
        elif r < 0.50:
            w("UPDATE t SET v=upper(v) WHERE id=%d;" % tid())
        elif r < 0.55:
            w("UPDATE s SET v=%s WHERE id=%d;" % (val(), rng.randint(1, NS + 1)))
        elif r < 0.60:
            i = tid()
            w("DELETE FROM t WHERE id=%d;" % i)
            if rng.random() < 0.7:
                w("INSERT INTO t VALUES(%d,%s);" % (i, val()))
                if rng.random() < 0.5:
                    w("UPDATE t SET v=%s WHERE id=%d;" % (val(), i))
        elif r < 0.63:
            w("INSERT OR REPLACE INTO t VALUES(%d,%s);" % (tid(), val()))
        elif r < 0.65:
            # rowid-changing update: a delete plus an insert, never loc==0
            i = tid()
            w("UPDATE OR REPLACE t SET id=%d WHERE id=%d;" % (rng.randint(1, NT * 3), i))
        elif r < 0.75:
            # changes the indexed column: the index delete is built from the OLD row
            w("UPDATE u SET a=%d, b=%s WHERE id=%d;"
              % (rng.randint(0, 60), val(), rng.randint(1, NU) * 2))
        elif r < 0.80:
            w("UPDATE u SET b=%s WHERE id=%d;" % (val(), rng.randint(1, NU) * 2))
        elif r < 0.84:
            # index-driven multi-row update (the cursor must keep its position)
            w("UPDATE u SET b=%s WHERE a=%d;" % (val(), rng.randint(0, 60)))
        elif r < 0.87:
            lo = tid()
            w("UPDATE t SET v=%s WHERE id BETWEEN %d AND %d;" % (val(), lo, lo + rng.randint(0, 40)))
        elif r < 0.92:
            w("DELETE FROM t WHERE id=%d;" % tid())
        else:
            w("INSERT OR IGNORE INTO t VALUES(%d,%s);" % (tid(), val()))

    def reads():
        w("SELECT count(*), total(length(v)) FROM t;")
        for _ in range(rng.randint(1, 3)):
            i = rng.choice(hot) if rng.random() < 0.5 else tid()
            w("SELECT id, v FROM t WHERE id=%d;" % i)
        lo = tid()
        w("SELECT id, v FROM t WHERE id>=%d ORDER BY id LIMIT 6;" % lo)
        w("SELECT id, v FROM t WHERE id<=%d ORDER BY id DESC LIMIT 6;" % lo)
        w("SELECT group_concat(id||':'||v) FROM s;")
        w("SELECT id, v FROM s WHERE id=%d;" % rng.randint(1, NS))
        a = rng.randint(0, 60)
        w("SELECT id, a, b FROM u WHERE a=%d ORDER BY id;" % a)
        w("SELECT count(*), total(a) FROM u;")

    for _ in range(n_txns):
        r = rng.random()
        if r < 0.45:
            w("BEGIN;")
            for _ in range(rng.randint(1, 12)):
                write_op()
            if rng.random() < 0.3:
                reads()                              # reads inside the txn
            w("COMMIT;")
        elif r < 0.60:
            w("BEGIN;")
            for _ in range(rng.randint(1, 12)):
                write_op()
            if rng.random() < 0.5:
                reads()
            w("ROLLBACK;")
        elif r < 0.70:
            w("BEGIN;")
            write_op()
            w("SAVEPOINT p1;")
            for _ in range(rng.randint(1, 6)):
                write_op()
            if rng.random() < 0.5:
                reads()
            w("ROLLBACK TO p1;")
            write_op()
            w("RELEASE p1;")
            w("COMMIT;" if rng.random() < 0.6 else "ROLLBACK;")
        elif r < 0.78 and journal == "wal":
            w(".output /dev/null")
            w("PRAGMA wal_checkpoint(%s);" % rng.choice(["PASSIVE", "TRUNCATE"]))
            w(".output stdout")
        else:
            for _ in range(rng.randint(1, 4)):       # autocommit
                write_op()
        reads()

    w("SELECT count(*), total(length(v)) FROM t;")
    w("PRAGMA integrity_check;")
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
