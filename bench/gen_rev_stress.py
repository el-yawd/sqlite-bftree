#!/usr/bin/env python3
"""Reverse-merge / COUNT differential workload (Stage 2.4).

gen_merge_stress.py only ever scans FORWARD, so the reverse merge path
(sqlite3BtreeLast + btreePrevious merging a leaf's buffered inserts with its
base cells backwards) and the merged sqlite3BtreeCount had no oracle coverage.
This generator emits, inside transactions that already hold buffered
BFOP_INSERT records and write-back tombstones:

  * full-table DESC scans           -> BtreeLast + BtreePrevious merge
  * `ORDER BY id DESC LIMIT k`      -> partial reverse scan (stops mid-stream)
  * `SELECT count(*)`               -> merged BtreeCount
  * `max(id)` / `min(id)`           -> single-step reverse / forward scans
  * DESC after DELETE               -> tombstone suppression in reverse
  * a DESC scan followed by an ASC scan on the same statement mix, so a
    direction flip inside one transaction is exercised too.

Run against the buffering fork (sqlite3_buf) and stock; query output + the
final .dump must match byte-for-byte.

Usage: gen_rev_stress.py <seed> [n_txns]
"""
import sys
import random

def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: gen_rev_stress.py <seed> [n_txns]\n")
        return 1
    seed = int(sys.argv[1])
    n_txns = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    rng = random.Random(seed)
    out = []
    w = out.append

    w("PRAGMA page_size=4096;")
    w("PRAGMA synchronous=OFF;")
    w("CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);")

    live = {}          # id -> value (exact model)
    id_hi = 1

    def rand_text():
        n = rng.randint(0, 60)
        return "".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(n))

    # Seed a committed base so reverse scans have real base cells to interleave.
    w("BEGIN;")
    for _ in range(rng.randint(20, 60)):
        i = id_hi; id_hi += 1
        v = rand_text(); live[i] = v
        w("INSERT INTO t VALUES(%d,'%s');" % (i, v))
    w("COMMIT;")

    for _ in range(n_txns):
        w("BEGIN;")
        # Brand-new inserts scattered across the active range: the reverse merge
        # must interleave them with base cells on many leaves, including the
        # very last leaf (BtreeLast's own leaf).
        for _ in range(rng.randint(1, 12)):
            if rng.random() < 0.5:
                i = id_hi; id_hi += 1
            else:
                i = rng.randint(1, id_hi + 5)
                while i in live:
                    i += 1
                id_hi = max(id_hi, i + 1)
            v = rand_text(); live[i] = v
            w("INSERT INTO t VALUES(%d,'%s');" % (i, v))

        # Reverse-scan shapes WHILE inserts are buffered.
        choice = rng.random()
        if choice < 0.35:
            w("SELECT id,v FROM t ORDER BY id DESC;")
        elif choice < 0.55:
            w("SELECT id,v FROM t ORDER BY id DESC LIMIT %d;" % rng.randint(1, 25))
        elif choice < 0.70:
            w("SELECT count(*), total(length(v)) FROM t;")
        elif choice < 0.85:
            w("SELECT 'MINMAX', min(id), max(id) FROM t;")
        else:
            # Direction flip inside one transaction, same buffered state.
            w("SELECT id FROM t ORDER BY id DESC;")
            w("SELECT id FROM t ORDER BY id;")

        # Deletes leave write-back tombstones the reverse scan must suppress.
        if live and rng.random() < 0.45:
            for _ in range(rng.randint(1, 3)):
                if not live:
                    break
                tid = rng.choice(tuple(live.keys()))
                del live[tid]
                w("DELETE FROM t WHERE id=%d;" % tid)
            w("SELECT id,v FROM t ORDER BY id DESC;")
            w("SELECT count(*) FROM t;")

        # Updates rewrite a buffered/base row; scan backwards over the result.
        if live and rng.random() < 0.35:
            tid = rng.choice(tuple(live.keys()))
            v = rand_text(); live[tid] = v
            w("UPDATE t SET v='%s' WHERE id=%d;" % (v, tid))
            w("SELECT id,v FROM t ORDER BY id DESC;")

        # Savepoint with a reverse scan inside, then roll it back.
        if rng.random() < 0.2:
            w("SAVEPOINT sp;")
            i = id_hi; id_hi += 1
            v = rand_text()
            w("INSERT INTO t VALUES(%d,'%s');" % (i, v))
            w("SELECT 'SP', max(id), count(*) FROM t;")
            w("SELECT id FROM t ORDER BY id DESC LIMIT 3;")
            w("ROLLBACK TO sp;")
            w("RELEASE sp;")
            # rolled back: no model change for the sp insert

        w("COMMIT;")

    w("PRAGMA integrity_check;")
    w("SELECT 'ROWSDESC', id, v FROM t ORDER BY id DESC;")
    w("SELECT 'ROWS', id, v FROM t ORDER BY id;")
    w("SELECT 'COUNT', count(*), total(length(v)), min(id), max(id) FROM t;")

    sys.stdout.write("\n".join(out) + "\n")
    return 0

if __name__ == "__main__":
    sys.exit(main())
