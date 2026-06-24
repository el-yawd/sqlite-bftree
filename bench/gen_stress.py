#!/usr/bin/env python3
"""Generate a deterministic random SQL workload for differential testing.

Given a seed and a journal mode, emit a self-contained SQL script on stdout
that exercises the code paths BF hooks into: rowid-table INSERT/UPDATE/DELETE,
point SELECTs, range scans, UNIQUE indexes (uniqueness checks + constraint
aborts), multi-statement transactions, and SAVEPOINT/ROLLBACK TO.

The same script is run against a stock SQLite build and the BF fork; their
query output and final `.dump` must match byte-for-byte (see stress.sh).  The
script therefore contains NO engine-specific pragmas (no PRAGMA bf_cache):
the fork applies BF by default, stock has it compiled out, and correct BF is
invisible to the observable result.

Usage: gen_stress.py <seed> <journal_mode> [n_ops]
"""
import sys
import random

def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: gen_stress.py <seed> <journal_mode> [n_ops]\n")
        return 1
    seed = int(sys.argv[1])
    journal = sys.argv[2]            # "wal", "delete", "memory", ...
    n_ops = int(sys.argv[3]) if len(sys.argv) > 3 else 4000
    rng = random.Random(seed)

    out = []
    w = out.append

    # Deterministic, durable-but-fast setup.  Journal mode is a parameter so
    # the suite covers both WAL and rollback-journal recovery paths.
    w("PRAGMA page_size=4096;")
    w("PRAGMA journal_mode=%s;" % journal)
    w("PRAGMA synchronous=OFF;")
    w("BEGIN;")
    # A rowid table (BF write-buffers these) with a UNIQUE secondary index
    # (exercises uniqueness-check descents that BF phantom-caches).
    w("CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER UNIQUE, v TEXT);")
    w("CREATE INDEX t_v ON t(v);")
    w("COMMIT;")

    # Model state so generated UPDATE/DELETE target rows that may exist and
    # UNIQUE inserts sometimes collide (to exercise constraint aborts).
    live_ids = set()
    used_k = set()
    next_id = 1

    def rand_text():
        n = rng.randint(0, 40)
        return "".join(rng.choice("abcdefghijklmnop") for _ in range(n))

    in_txn = False
    savepoint_depth = 0

    for _ in range(n_ops):
        r = rng.random()

        # Transaction control, interleaved with data ops.
        if not in_txn and r < 0.10:
            w("BEGIN;")
            in_txn = True
            continue
        if in_txn and r < 0.04:
            w("SAVEPOINT s%d;" % savepoint_depth)
            savepoint_depth += 1
            continue
        if in_txn and savepoint_depth > 0 and r < 0.07:
            savepoint_depth -= 1
            # Roll back to (and release) the most recent savepoint.  We do not
            # try to model the state reversal precisely; instead we snapshot
            # by simply not trusting live_ids after a rollback path.  To keep
            # the model exact, mirror by re-reading is overkill — so we avoid
            # divergence by making the rolled-back span side-effect free:
            # emit only a ROLLBACK TO followed by RELEASE, with no model edit
            # inside the span (the span above only added a SAVEPOINT).
            w("ROLLBACK TO s%d;" % savepoint_depth)
            w("RELEASE s%d;" % savepoint_depth)
            continue
        if in_txn and r < 0.16:
            w("COMMIT;")
            in_txn = False
            savepoint_depth = 0
            continue

        op = rng.random()
        if op < 0.45:
            # INSERT.  ~15% of the time reuse an existing k to provoke a
            # UNIQUE collision; wrap those so the abort doesn't kill the run.
            iid = next_id
            next_id += 1
            if used_k and rng.random() < 0.15:
                k = rng.choice(tuple(used_k))   # likely collision -> abort
                w("INSERT OR IGNORE INTO t VALUES(%d,%d,'%s');"
                  % (iid, k, rand_text()))
                # OR IGNORE: row inserted only if k was actually free; it
                # wasn't (k in used_k) so no state change in the common case.
                # If used_k lost k via delete, it may insert — model that:
                if k not in used_k:
                    live_ids.add(iid); used_k.add(k)
            else:
                k = rng.randint(1, 10_000_000)
                while k in used_k:
                    k += 1
                w("INSERT INTO t VALUES(%d,%d,'%s');" % (iid, k, rand_text()))
                live_ids.add(iid); used_k.add(k)
        elif op < 0.60 and live_ids:
            # UPDATE the payload (no key change -> no UNIQUE interaction).
            tid = rng.choice(tuple(live_ids))
            w("UPDATE t SET v='%s' WHERE id=%d;" % (rand_text(), tid))
        elif op < 0.72 and live_ids:
            # DELETE.
            tid = rng.choice(tuple(live_ids))
            w("DELETE FROM t WHERE id=%d;" % tid)
            live_ids.discard(tid)
        elif op < 0.88:
            # Point SELECT (hits BF read path / phantom path).
            tid = rng.randint(1, max(1, next_id + 50))
            w("SELECT id,k,v FROM t WHERE id=%d;" % tid)
        else:
            # Range scan (forces BF scan-prep flush).
            lo = rng.randint(1, max(1, next_id))
            w("SELECT id,k FROM t WHERE id>=%d ORDER BY id LIMIT 20;" % lo)

    if in_txn:
        w("COMMIT;")

    # Final consistency oracle: integrity check + a full ordered dump of
    # every table, so the comparison covers the whole database image.
    w("PRAGMA integrity_check;")
    w("SELECT 'ROWS', id, k, v FROM t ORDER BY id;")
    w("SELECT 'BYV', id, v FROM t ORDER BY v, id;")
    w("SELECT 'COUNT', count(*), sum(k), total(length(v)) FROM t;")

    sys.stdout.write("\n".join(out) + "\n")
    return 0

if __name__ == "__main__":
    sys.exit(main())
