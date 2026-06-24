#!/usr/bin/env python3
"""Write-back delete differential workload (Stage 2.3).

Targets the cases the existing generators do NOT reach:
  * DELETE id=X then re-INSERT id=X within the SAME transaction.  Write-back
    leaves the base cell in place and buffers a BFOP_DELETE; the re-insert
    overwrites that tombstone in-place with a BFOP_INSERT, so a base cell and a
    buffered insert now share a key (the "shadow" the merge scan must collapse).
  * DELETE of a row that exists only as a buffered insert (net "no row").
  * Full forward (ORDER BY id) AND reverse (ORDER BY id DESC) scans plus
    count(*) WHILE tombstones/inserts are buffered, driving merge tombstone
    suppression and the reverse-scan fallback.

Keeps an exact Python model of live rows so every emitted statement is valid and
the expected query output is deterministic.  Run against sqlite3_buf and stock;
query output + final .dump must match byte-for-byte.

Usage: gen_wb_delete_stress.py <seed> [n_txns]
"""
import sys
import random

def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: gen_wb_delete_stress.py <seed> [n_txns]\n")
        return 1
    seed = int(sys.argv[1])
    n_txns = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    rng = random.Random(seed)
    out = []
    w = out.append

    w("PRAGMA page_size=4096;")
    w("PRAGMA synchronous=OFF;")
    w("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);")

    live = {}          # id -> value (exact model)
    id_hi = 1

    def rand_text():
        n = rng.randint(0, 80)
        return "".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(n))

    def scan():
        c = rng.random()
        if c < 0.45:
            w("SELECT id,v FROM t ORDER BY id;")
        elif c < 0.75:
            w("SELECT id,v FROM t ORDER BY id DESC;")   # reverse: merge bails
        else:
            w("SELECT count(*), total(length(v)), max(id), min(id) FROM t;")

    # Committed base so scans interleave with real base cells.
    w("BEGIN;")
    for _ in range(rng.randint(10, 50)):
        i = id_hi; id_hi += 1
        v = rand_text(); live[i] = v
        w("INSERT INTO t VALUES(%d,'%s');" % (i, v))
    w("COMMIT;")

    for _ in range(n_txns):
        w("BEGIN;")
        # Burst of brand-new inserts scattered across the id range.
        for _ in range(rng.randint(1, 10)):
            if rng.random() < 0.5:
                i = id_hi; id_hi += 1
            else:
                i = rng.randint(1, id_hi + 5)
                while i in live:
                    i += 1
                id_hi = max(id_hi, i + 1)
            v = rand_text(); live[i] = v
            w("INSERT INTO t VALUES(%d,'%s');" % (i, v))

        scan()

        # Delete some live rows (base rows AND just-buffered inserts).
        for _ in range(rng.randint(0, 4)):
            if not live:
                break
            tid = rng.choice(tuple(live.keys()))
            del live[tid]
            w("DELETE FROM t WHERE id=%d;" % tid)
        scan()

        # Re-insert a just-deleted key in THIS txn (the shadow path): pick an id
        # below id_hi not currently live and (re)insert it.
        if rng.random() < 0.6:
            for _try in range(8):
                cand = rng.randint(1, id_hi)
                if cand not in live:
                    v = rand_text(); live[cand] = v
                    w("INSERT INTO t VALUES(%d,'%s');" % (cand, v))
                    break
            scan()

        # Savepoint span: delete + reinsert inside, then roll back (no model
        # change), exercising tombstone rollback.
        if live and rng.random() < 0.25:
            w("SAVEPOINT sp;")
            tid = rng.choice(tuple(live.keys()))
            w("DELETE FROM t WHERE id=%d;" % tid)
            w("INSERT INTO t VALUES(%d,'%s');" % (tid, rand_text()))
            w("SELECT count(*) FROM t;")
            w("ROLLBACK TO sp;")
            w("RELEASE sp;")

        w("COMMIT;")
        if rng.random() < 0.2:
            scan()   # post-commit scan (clean base)

    w("PRAGMA integrity_check;")
    w("SELECT 'ROWS', id, v FROM t ORDER BY id;")
    w("SELECT 'COUNT', count(*), total(length(v)) FROM t;")

    sys.stdout.write("\n".join(out) + "\n")
    return 0

if __name__ == "__main__":
    sys.exit(main())
