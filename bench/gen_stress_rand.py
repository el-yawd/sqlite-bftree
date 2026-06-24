#!/usr/bin/env python3
"""Random-ROWID differential workload — exposes BF per-leaf cache coherence bugs.

Unlike gen_stress.py (which inserts MONOTONIC rowids, so a leaf split always
pushes the new key to a fresh page and hides per-leaf staleness), this inserts
RANDOM rowids in a small range with INSERT OR REPLACE + UPDATE + DELETE, so rows
land in the middle of leaves, leaves split/merge, and pgnos get recycled.  Run
the BF fork vs stock and diff query output + integrity_check + final dump.

Usage: gen_stress_rand.py <seed> <n_ops>   (e.g. 3000)
Driver (ad hoc):
  for s in $(seq 1 20); do
    python3 bench/gen_stress_rand.py $s 3000 > /tmp/r.sql
    ../build/sqlite3 /tmp/bf.db < /tmp/r.sql > /tmp/bf.out 2>&1
    ../build/sqlite3_stock /tmp/st.db < /tmp/r.sql > /tmp/st.out 2>&1
    diff -q /tmp/st.out /tmp/bf.out || echo "seed $s FAIL"
  done
Set range to 400 with deletes to reproduce; dropping deletes makes it PASS
(implicating page merge/free + pgno reuse, though the freePage2 hook alone did
NOT fix it — see memory bf-stage1.2-coherence).
"""
import sys, random

def main():
    seed = int(sys.argv[1]); n = int(sys.argv[2]); rng = random.Random(seed)
    out = ["PRAGMA page_size=4096;",
           "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);"]
    live = set()
    def txt(): return ''.join(rng.choice('abcdefgh') for _ in range(rng.randint(1, 60)))
    for _ in range(n):
        r = rng.random()
        if r < 0.40:
            k = rng.randint(1, 400)               # random, non-monotonic, collisions
            out.append("INSERT OR REPLACE INTO t VALUES(%d,'%s');" % (k, txt())); live.add(k)
        elif r < 0.55 and live:
            k = rng.choice(tuple(live)); out.append("DELETE FROM t WHERE id=%d;" % k); live.discard(k)
        elif r < 0.70 and live:
            k = rng.choice(tuple(live)); out.append("UPDATE t SET v='%s' WHERE id=%d;" % (txt(), k))
        elif r < 0.88:
            k = rng.randint(1, 400); out.append("SELECT id,v FROM t WHERE id=%d;" % k)
        else:
            lo = rng.randint(1, 400); out.append("SELECT id,v FROM t WHERE id>=%d ORDER BY id LIMIT 15;" % lo)
    out.append("PRAGMA integrity_check;")
    out.append("SELECT 'ALL',id,v FROM t ORDER BY id;")
    sys.stdout.write("\n".join(out) + "\n")

if __name__ == "__main__":
    main()
