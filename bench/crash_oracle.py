#!/usr/bin/env python3
"""D1 crash-differential oracle: multi-root workloads, SIGKILL at a random
commit, recovery compared against the set of ALLOWED committed states.

For each seed:
  1. generate K transactions over several rowid tables (random-rowid inserts,
     range deletes, same-size and size-changing updates, overflow values,
     a secondary-indexed table, a CREATE TABLE mid-stream so roots appear late);
  2. run them on STOCK once, printing sha3 of the full logical state after
     every COMMIT -> H[0..K];
  3. run them on BF under stdbuf -oL, and SIGKILL right after marker m is seen.
     Every commit up to m was acknowledged, so the recovered state must equal
     H[j] for some j >= m (later commits may have landed before the kill);
  4. recover under one of several orders (read first / checkpoint first /
     checkpoint in a separate process / crash DURING the checkpoint), and
     gate on hash membership AND integrity_check at every step;
  5. replay transactions j+1..K on the recovered database and require H[K].

What it found on its first runs (2026-09-24), none visible to the existing
oracles:
  * ANY full ROLLBACK destroyed committed buffered rows (BEGIN;ROLLBACK was
    enough) -- sqlite3BtreeRollback emptied the record cache;
  * recovery replayed record ops over NEWER page images of their leaf: an
    UPDATE/DELETE that reached base through the page path came undone at the
    next checkpoint;
  * recovery replayed the record frames of a TORN commit;
  * group commit deferred the records of a commit that also wrote pages, so a
    crash kept half the transaction ("wrong # of entries in index");
  * a torn tail starting with a record frame made the next write drop one of its
    own wal-index entries (bench/torn_tail_repro.sh is the deterministic form).

Usage (from bench/):
    python3 crash_oracle.py [seeds...]                      # default knobs
    BF_PRAGMAS="PRAGMA bf_cache_size=262144;" python3 crash_oracle.py 1 2 3
    BF_GROUP=8 python3 crash_oracle.py 1 2 3                # group commit
    MODES=read,ckpt NTX=40 BUF=... STOCK=... WORK=/tmp/x python3 crash_oracle.py
A failing run keeps its database (and .sql) in WORK for diagnosis.
"""
import os, random, signal, subprocess, sys, time, tempfile

BUF = os.environ.get("BUF", "../build/sqlite3_buf")
STOCK = os.environ.get("STOCK", "../build/sqlite3_stock")
BFPRAGMAS = os.environ.get("BF_PRAGMAS", "")   # e.g. "PRAGMA bf_cache_size=262144;"
WORK = os.environ.get("WORK", tempfile.gettempdir())
# PRAGMA bf_group_commit=N defers up to N-1 acknowledged commits (bounded
# deferred durability, BF_TREE_V2_PLAN.md D2), so a crash may lose that many.
GROUP = int(os.environ.get("BF_GROUP", "1"))
if GROUP > 1:
    BFPRAGMAS = (BFPRAGMAS + " " if BFPRAGMAS else "") + f"PRAGMA bf_group_commit={GROUP};"

HASHQ_TABLES = ["t1", "t2", "t3", "t4"]


def hash_sql(tables):
    # t4 takes auto-assigned rowids, and BF's buffered max-rowid is monotonic
    # (bf_cache.h: "never cleared"), so after a rollback it skips rowids stock
    # reuses.  That is a documented deviation, not data loss: hash t4 by
    # content.  The quotes are doubled because the whole query is itself a
    # single-quoted SQL string.
    def q(t):
        if t == "t4":
            return "SELECT ''t4'', a, b FROM t4 ORDER BY a, b;"
        return f"SELECT ''{t}'', * FROM {t} ORDER BY rowid;"
    return "SELECT hex(sha3_query('" + " ".join(q(t) for t in tables) + "'));"


def gen(seed, ntx):
    r = random.Random(seed)
    txs = []
    live = {"t1": set(), "t2": set(), "t3": set()}
    tables = ["t1", "t2", "t4"]          # t3 is created mid-stream
    t3_at = r.randint(ntx // 4, ntx // 2)
    ids4 = 0

    def val(n=None):
        n = n if n is not None else r.choice([8, 30, 30, 60, 120, 400, 2500])
        return "'" + "".join(r.choice("abcdefghij") for _ in range(min(n, 40))) + \
               "'" + (f"||hex(zeroblob({n}))" if n > 40 else "")

    for k in range(1, ntx + 1):
        stmts = []
        if k == t3_at:
            stmts.append("CREATE TABLE t3(id INTEGER PRIMARY KEY, v TEXT);")
            tables.append("t3")
        nops = r.choice([1, 1, 5, 20, 80, 200])
        for _ in range(nops):
            t = r.choice(tables)
            op = r.random()
            if t == "t4":
                ids4 += 1
                if op < 0.8:
                    stmts.append(f"INSERT INTO t4(a,b) VALUES({r.randint(0,999)},{val()});")
                else:
                    stmts.append(f"DELETE FROM t4 WHERE a BETWEEN {r.randint(0,999)} AND {r.randint(0,999)} ;")
                continue
            s = live[t]
            if op < 0.6 or not s:
                key = r.randint(1, 20000) if r.random() < 0.7 else (max(s) + 1 if s else 1)
                stmts.append(f"INSERT OR REPLACE INTO {t} VALUES({key},{val()});")
                s.add(key)
            elif op < 0.75:
                key = r.choice(sorted(s))
                stmts.append(f"UPDATE {t} SET v={val()} WHERE id={key};")
            elif op < 0.85:
                key = r.choice(sorted(s))
                stmts.append(f"UPDATE {t} SET v=substr(v,1,length(v)) WHERE id={key};")
            elif op < 0.95:
                key = r.choice(sorted(s))
                stmts.append(f"DELETE FROM {t} WHERE id={key};")
                s.discard(key)
            else:
                lo = r.randint(1, 20000); hi = lo + r.randint(0, 500)
                stmts.append(f"DELETE FROM {t} WHERE id BETWEEN {lo} AND {hi};")
                for x in [x for x in s if lo <= x <= hi]:
                    s.discard(x)
        if r.random() < 0.15:
            stmts.append("ROLLBACK_MARK")   # an aborted txn before this one
        txs.append(stmts)
    return txs


HEADER = ["PRAGMA journal_mode=wal;", "PRAGMA wal_autocheckpoint=0;"]
SCHEMA = ["CREATE TABLE IF NOT EXISTS t1(id INTEGER PRIMARY KEY, v TEXT);",
          "CREATE TABLE IF NOT EXISTS t2(id INTEGER PRIMARY KEY, v TEXT);",
          "CREATE TABLE IF NOT EXISTS t4(id INTEGER PRIMARY KEY, a INT, b TEXT);",
          "CREATE INDEX IF NOT EXISTS t4a ON t4(a);"]


def tx_sql(k, stmts, tables_for_hash, with_hash):
    out = []
    body = [s for s in stmts if s != "ROLLBACK_MARK"]
    if "ROLLBACK_MARK" in stmts:
        out.append("BEGIN;")
        out += body[: max(1, len(body) // 2)]
        out.append("ROLLBACK;")
    out.append("BEGIN;")
    out += body
    out.append("COMMIT;")
    out.append(f".print M{k}")
    if with_hash:
        out.append(hash_sql(tables_for_hash(k)))
    return out


def run(binary, db, sql, timeout=600):
    p = subprocess.run([binary, db], input=sql, capture_output=True, text=True, timeout=timeout)
    return p.stdout + p.stderr


def main():
    seeds = [int(x) for x in sys.argv[1:]] or [1, 2, 3]
    ntx = int(os.environ.get("NTX", "40"))
    modes = os.environ.get("MODES", "read,ckpt,sepckpt,killckpt").split(",")
    fails = 0; total = 0
    for seed in seeds:
        txs = gen(seed, ntx)
        created = next((i + 1 for i, t in enumerate(txs) if any(s.startswith("CREATE TABLE t3") for s in t)), ntx + 1)
        tabs = lambda k: HASHQ_TABLES if k >= created else ["t1", "t2", "t4"]
        # ---- stock: H[k] for every prefix k ----
        sdb = os.path.join(WORK, f"crash_st_{seed}.db")
        for suf in ("", "-wal", "-shm"):
            try: os.remove(sdb + suf)
            except FileNotFoundError: pass
        script = HEADER + SCHEMA + [".print M0", hash_sql(tabs(0))]
        for k, st in enumerate(txs, 1):
            script += tx_sql(k, st, tabs, True)
        out = run(STOCK, sdb, "\n".join(script) + "\n").split("\n")
        H = {}
        for i, line in enumerate(out):
            if line.startswith("M") and line[1:].isdigit():
                H[int(line[1:])] = out[i + 1]
        assert len(H) == ntx + 1, f"stock produced {len(H)} hashes: {out[-5:]}"
        hv = {v: k for k, v in H.items()}
        for mode in modes:
            total += 1
            r = random.Random(seed * 1000 + sum(map(ord, mode)))
            target = r.randint(1, ntx)
            db = os.path.join(WORK, f"crash_bf_{seed}_{mode}.db")
            for suf in ("", "-wal", "-shm"):
                try: os.remove(db + suf)
                except FileNotFoundError: pass
            script = HEADER + ([BFPRAGMAS] if BFPRAGMAS else []) + SCHEMA
            for k, st in enumerate(txs, 1):
                script += tx_sql(k, st, tabs, False)
            script += [".print END"] + ["SELECT 1;"] * 200000
            with open(db + ".sql", "w") as f:
                f.write("\n".join(script) + "\n")
            p = subprocess.Popen(["stdbuf", "-oL", BUF, db], stdin=open(db + ".sql"),
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            seen = 0; errs = []
            for line in p.stdout:
                line = line.rstrip("\n")
                if line.startswith("M") and line[1:].isdigit():
                    seen = int(line[1:])
                    if seen >= target:
                        break
                elif line == "END":
                    break
                elif line and line not in ("1", "wal", "0"):
                    errs.append(line)
            p.send_signal(signal.SIGKILL); p.wait()
            status = []
            ok = True
            if errs:
                status.append(f"BF load errors: {errs[:3]}"); ok = False

            def probe(label, pre=None):
                nonlocal ok
                if pre:
                    run(BUF, db, pre)
                h = run(BUF, db, hash_sql(HASHQ_TABLES if os.path.exists(db) and "t3" in run(BUF, db, ".tables") else ["t1", "t2", "t4"])).strip()
                ic = run(BUF, db, "PRAGMA integrity_check;").strip().split("\n")[0]
                j = hv.get(h)
                if j is None or j < seen - (GROUP - 1) or ic != "ok":
                    ok = False
                    status.append(f"{label}: state={'prefix %d' % j if j is not None else 'NOT A COMMITTED STATE'} seen={seen} ic={ic}")
                else:
                    status.append(f"{label}: prefix {j}")
                return j

            if mode == "read":
                j = probe("after-read")
                j = probe("after-ckpt", "PRAGMA wal_checkpoint(TRUNCATE);")
            elif mode == "ckpt":
                run(BUF, db, "PRAGMA wal_checkpoint(TRUNCATE);")
                j = probe("ckpt-first")
            elif mode == "sepckpt":
                run(BUF, db, "SELECT 1;")
                run(BUF, db, "PRAGMA wal_checkpoint(TRUNCATE);")
                j = probe("sep-ckpt")
            elif mode == "killckpt":
                wal = os.path.getsize(db + "-wal") if os.path.exists(db + "-wal") else 0
                cp = subprocess.Popen([BUF, db, "PRAGMA wal_checkpoint(TRUNCATE);"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(r.uniform(0.0, 0.03))
                cp.send_signal(signal.SIGKILL); cp.wait()
                status.append(f"wal={wal}B")
                j = probe("after-killed-ckpt")
                j = probe("after-2nd-ckpt", "PRAGMA wal_checkpoint(TRUNCATE);")
            # ---- continue: apply j+1..K, require H[K] ----
            if ok and j is not None:
                rest = []
                for k in range(j + 1, ntx + 1):
                    rest += tx_sql(k, txs[k - 1], tabs, False)
                o = run(BUF, db, "\n".join(HEADER + ([BFPRAGMAS] if BFPRAGMAS else []) + rest) + "\n")
                bad = [l for l in o.split("\n") if l and not l.startswith("M") and l not in ("wal", "0")]
                h = run(BUF, db, hash_sql(HASHQ_TABLES)).strip()
                ic = run(BUF, db, "PRAGMA integrity_check;").strip().split("\n")[0]
                if h != H[ntx] or ic != "ok" or bad:
                    ok = False
                    status.append(f"continue: final {'OK' if h == H[ntx] else 'WRONG (prefix %s)' % hv.get(h)} ic={ic} err={bad[:2]}")
                else:
                    status.append("continue: final OK")
            print(("PASS " if ok else "FAIL ") + f"seed={seed} mode={mode} kill@M{seen}: " + "; ".join(status), flush=True)
            if not ok:
                fails += 1
            else:
                for suf in ("", "-wal", "-shm", ".sql"):
                    try: os.remove(db + suf)
                    except FileNotFoundError: pass
    print(f"\n{'ALL CLEAN' if fails == 0 else 'FAILURES'}: {total - fails}/{total} passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
