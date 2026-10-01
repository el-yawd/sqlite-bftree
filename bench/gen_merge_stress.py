#!/usr/bin/env python3
"""Merge-scan differential workload (Stage 2.2).

Unlike gen_stress.py (whose in-transaction SELECTs are point/range lookups, so
the only full scans run post-commit with nothing buffered), this generator
emits FULL-TABLE FORWARD SCANS *inside* open transactions that already hold
buffered BFOP_INSERT records.  That is exactly the path Stage 2.2 merge-
iteration takes: btreeNext interleaving a leaf's buffered inserts with its base
cells.  Run against the buffering fork (sqlite3_buf) and stock; query output +
final .dump must match byte-for-byte.

The workload keeps an exact Python model of live rows so every statement is
valid and non-aborting, and so buffered inserts use brand-new rowids (the only
shape the insert hook buffers).  Inserts deliberately use rowids that fall
*between* existing rows (random within the active id range) so a full scan must
interleave them — the merge's whole job.

Negative rowids (2026-10-01): a third of the seeds shift every key down by
KOFF so the table straddles zero.  BF stored rowid keys as plain big-endian
two's complement, which sorts -1 AFTER +5; a buffered negative row came back
last from ORDER BY id and was missed by WHERE id<0, and no generator ever
produced one.  KOFF and the sign-boundary queries draw from their own RNG, so
seeds with KOFF==0 emit exactly the script they always did.

Usage: gen_merge_stress.py <seed> [n_txns]
"""
import sys
import random

def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: gen_merge_stress.py <seed> [n_txns]\n")
        return 1
    seed = int(sys.argv[1])
    n_txns = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    rng = random.Random(seed)
    rng2 = random.Random(seed * 7919 + 17)
    koff = -rng2.randint(5, 600) if rng2.random() < 0.34 else 0
    out = []
    w = out.append

    def K(i):                       # model id -> SQL rowid
        return i + koff

    w("PRAGMA page_size=4096;")
    w("PRAGMA synchronous=OFF;")
    w("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);")

    live = {}          # id -> value (exact model)
    id_hi = 1

    def rand_text():
        n = rng.randint(0, 60)
        return "".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(n))

    # Seed a committed base so scans have real base cells to interleave with.
    w("BEGIN;")
    for _ in range(rng.randint(5, 40)):
        i = id_hi; id_hi += 1
        v = rand_text(); live[i] = v
        w("INSERT INTO t VALUES(%d,'%s');" % (K(i), v))
    w("COMMIT;")

    for _ in range(n_txns):
        w("BEGIN;")
        # A burst of brand-new inserts with rowids scattered across the active
        # range so they land between existing base cells on various leaves.
        for _ in range(rng.randint(1, 12)):
            # New unique id: sometimes append at the top, often insert into a
            # gap below id_hi to force mid-leaf interleaving.
            if rng.random() < 0.5:
                i = id_hi; id_hi += 1
            else:
                i = rng.randint(1, id_hi + 5)
                while i in live:
                    i += 1
                id_hi = max(id_hi, i + 1)
            v = rand_text(); live[i] = v
            w("INSERT INTO t VALUES(%d,'%s');" % (K(i), v))

        # Full-table forward scans WHILE inserts are buffered (drives merge).
        choice = rng.random()
        if choice < 0.5:
            w("SELECT id,v FROM t ORDER BY id;")
        elif choice < 0.7:
            w("SELECT count(*), total(length(v)) FROM t;")
        else:
            # Aggregate that still forces a full ordered walk.
            w("SELECT 'SUMID', sum(id), min(id), max(id) FROM t;")
        if koff:
            # Sign-boundary seeks and scans, both directions, while buffered.
            b = rng2.randint(-3, 3)
            w("SELECT group_concat(id) FROM t WHERE id<%d;" % b)
            w("SELECT id FROM t WHERE id>=%d ORDER BY id LIMIT 8;" % b)
            w("SELECT id FROM t WHERE id<=%d ORDER BY id DESC LIMIT 8;" % b)
            w("SELECT group_concat(id) FROM (SELECT id FROM t ORDER BY id DESC);")

        # Occasionally update/delete (these flush the buffer mid-txn, exercising
        # the merge->plain transition), then scan again.
        if live and rng.random() < 0.4:
            tid = rng.choice(tuple(live.keys()))
            if rng.random() < 0.5:
                v = rand_text(); live[tid] = v
                w("UPDATE t SET v='%s' WHERE id=%d;" % (v, K(tid)))
            else:
                del live[tid]
                w("DELETE FROM t WHERE id=%d;" % K(tid))
            w("SELECT id,v FROM t ORDER BY id;")   # post-mutation full scan

        # Savepoint with a scan inside, then release (side-effect free span).
        if rng.random() < 0.2:
            w("SAVEPOINT sp;")
            i = id_hi; id_hi += 1
            v = rand_text()
            w("INSERT INTO t VALUES(%d,'%s');" % (K(i), v))
            w("SELECT count(*) FROM t;")           # scan with sp-buffered insert
            w("ROLLBACK TO sp;")
            w("RELEASE sp;")
            # rolled back: no model change for the sp insert

        w("COMMIT;")

    w("PRAGMA integrity_check;")
    w("SELECT 'ROWS', id, v FROM t ORDER BY id;")
    w("SELECT 'COUNT', count(*), total(length(v)) FROM t;")

    sys.stdout.write("\n".join(out) + "\n")
    return 0

if __name__ == "__main__":
    sys.exit(main())
