#!/usr/bin/env python3
"""Full-ROLLBACK differential workload (D1, 2026-09-24).

Every other generator in this directory rolls back only with ROLLBACK TO, which
goes through the flush-before-savepoint protocol, and none of them lets a
statement fail.  So none of them ever reached sqlite3BtreeRollback with
committed records still buffered -- and that path emptied the record cache,
destroying COMMITTED rows that lived only in their mini-page and WAL record
frame.  BEGIN;ROLLBACK with no writes was enough; so was an autocommit INSERT
that failed a UNIQUE constraint.  The suite stayed green for the project's whole
life because it structurally never asked.

This one asks, in every shape: committed transactions interleaved with
  * write transactions that end in ROLLBACK (inserts, updates of both sizes,
    deletes, overflow values, a secondary-indexed table),
  * empty BEGIN;ROLLBACK and read-only BEGIN;SELECT;ROLLBACK,
  * COMMIT with no transaction open (fails),
  * autocommit statements that fail a constraint, and failing statements
    inside a transaction that then commits,
  * savepoints rolled back to and then the whole transaction rolled back,
  * mid-session checkpoints (output discarded: engine-dependent frame counts),
with reads after every transaction.  Error messages are part of the compared
output, so both engines must fail the same statements the same way.

No exact model is kept: both engines run the same script, and the oracle
compares their outputs and final .dump.  Explicit rowids everywhere -- BF's
buffered max-rowid is monotonic by design (bf_cache.h), so auto-assigned rowids
after a rollback may differ from stock without being wrong.

Usage: gen_rollback_stress.py <seed> [n_txns] [journal]
"""
import random
import sys


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: gen_rollback_stress.py <seed> [n_txns] [journal]\n")
        return 1
    seed = int(sys.argv[1])
    n_txns = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    journal = sys.argv[3] if len(sys.argv) > 3 else "wal"
    rng = random.Random(seed)
    out = []
    w = out.append

    w("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);")
    w("CREATE TABLE u(id INTEGER PRIMARY KEY, a INT, b TEXT);")
    w("CREATE INDEX ua ON u(a);")

    ids_t = set()      # ids that MAY exist (superset after rollbacks is fine)
    ids_u = set()
    hi = [1]

    def text(n=None):
        n = rng.choice([0, 3, 12, 30, 60]) if n is None else n
        return "".join(rng.choice("abcdefghijklmnop") for _ in range(n))

    def val():
        r = rng.random()
        if r < 0.05:
            return "'%s'||hex(zeroblob(%d))" % (text(8), rng.choice([900, 3000]))
        return "'%s'" % text()

    def new_id():
        # Mostly gaps inside the populated range (buffered mid-leaf), some appends.
        if ids_t and rng.random() < 0.7:
            return rng.randint(1, hi[0] * 3)
        hi[0] += rng.randint(1, 4)
        return hi[0] * 3

    def some(ids):
        return rng.choice(sorted(ids)) if ids else rng.randint(1, 100)

    def write_op():
        r = rng.random()
        if r < 0.40:
            i = new_id()
            w("INSERT OR REPLACE INTO t VALUES(%d,%s);" % (i, val()))
            ids_t.add(i)
        elif r < 0.52:
            w("UPDATE t SET v=%s WHERE id=%d;" % (val(), some(ids_t)))
        elif r < 0.60:
            # same-size overwrite: the in-place path
            w("UPDATE t SET v=upper(v) WHERE id=%d;" % some(ids_t))
        elif r < 0.70:
            w("DELETE FROM t WHERE id=%d;" % some(ids_t))
        elif r < 0.74:
            lo = some(ids_t)
            w("DELETE FROM t WHERE id BETWEEN %d AND %d;" % (lo, lo + rng.randint(0, 30)))
        elif r < 0.90:
            i = rng.randint(1, 5000)
            w("INSERT OR REPLACE INTO u VALUES(%d,%d,%s);" % (i, rng.randint(0, 99), val()))
            ids_u.add(i)
        elif r < 0.95:
            w("DELETE FROM u WHERE a=%d;" % rng.randint(0, 99))
        else:
            w("UPDATE u SET b=%s WHERE a=%d;" % (val(), rng.randint(0, 99)))

    def reads():
        w("SELECT count(*), total(id), total(length(v)) FROM t;")
        w("SELECT count(*), total(a) FROM u;")
        for _ in range(rng.randint(1, 3)):
            w("SELECT id, v FROM t WHERE id=%d;" % some(ids_t))
        lo = some(ids_t)
        w("SELECT id FROM t WHERE id>=%d ORDER BY id LIMIT 8;" % lo)
        w("SELECT id FROM t WHERE id<=%d ORDER BY id DESC LIMIT 8;" % lo)
        a = rng.randint(0, 99)
        w("SELECT id, b FROM u WHERE a=%d ORDER BY id;" % a)

    # A committed base with several leaves.
    w("BEGIN;")
    for _ in range(rng.randint(80, 300)):
        i = new_id()
        w("INSERT OR REPLACE INTO t VALUES(%d,%s);" % (i, val()))
        ids_t.add(i)
    w("COMMIT;")

    for _ in range(n_txns):
        r = rng.random()
        if r < 0.40:                                   # committed write txn
            w("BEGIN;")
            for _ in range(rng.randint(1, 15)):
                write_op()
            w("COMMIT;")
        elif r < 0.55:                                 # rolled-back write txn
            w("BEGIN;")
            for _ in range(rng.randint(1, 15)):
                write_op()
            if rng.random() < 0.5:
                reads()                                # reads see the txn's writes
            w("ROLLBACK;")
        elif r < 0.60:                                 # empty
            w("BEGIN;")
            w("ROLLBACK;")
        elif r < 0.64:                                 # read-only
            w("BEGIN;")
            w("SELECT count(*) FROM t;")
            w("ROLLBACK;")
        elif r < 0.67:                                 # no transaction: fails
            w("COMMIT;")
        elif r < 0.73:                                 # autocommit constraint failure
            w("INSERT INTO t VALUES(%d,'dup');" % some(ids_t))
        elif r < 0.78:                                 # failure inside a committed txn
            w("BEGIN;")
            write_op()
            w("INSERT INTO t VALUES(%d,'dup');" % some(ids_t))
            write_op()
            w("COMMIT;")
        elif r < 0.86:                                 # savepoint, then abandon it all
            w("BEGIN;")
            write_op()
            w("SAVEPOINT s1;")
            for _ in range(rng.randint(1, 6)):
                write_op()
            w("ROLLBACK TO s1;")
            write_op()
            w("RELEASE s1;")
            w("ROLLBACK;" if rng.random() < 0.6 else "COMMIT;")
        elif r < 0.90 and journal == "wal":            # mid-session checkpoint
            w(".output /dev/null")
            w("PRAGMA wal_checkpoint(%s);" % rng.choice(["PASSIVE", "TRUNCATE"]))
            w(".output stdout")
        else:                                          # autocommit writes
            for _ in range(rng.randint(1, 5)):
                write_op()
        reads()

    w("PRAGMA integrity_check;")
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
