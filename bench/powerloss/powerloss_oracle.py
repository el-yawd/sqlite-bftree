#!/usr/bin/env python3
"""Power-loss oracle (D2, 2026-09-24): what survives a power cut, per mode?

bench/crash_oracle.py SIGKILLs the process, which cannot lose a write that
reached the kernel -- so it says nothing about PRAGMA synchronous.  This one runs
the same workloads through plvfs (a shim VFS that keeps an undo log of every
write since each file's last sync) and cuts the power at a random I/O event:
unsynced writes are reverted -- all, a random subset, or torn at sector
granularity -- and the process dies.  Then the database is reopened normally.

Gates, per run:
  * the recovered state equals stock's state after SOME commit prefix j
    (atomicity: never a torn transaction), and integrity_check is ok;
  * j >= the durability bound: the last commit k acknowledged while no file had
    an unsynced write (plvfs flags the marker " D").  Under synchronous=FULL
    with bf_deferred_commit<=1 that is every acknowledged commit; under NORMAL
    it is none -- not even the schema (j=-1) -- since NORMAL promises atomicity,
    not durability, across a power loss;
  * after a checkpoint the state is unchanged and still passes integrity_check.

Usage (from bench/powerloss):
    cc -O2 -DSQLITE_BF_INSERT_BUFFERING -DSQLITE_THREADSAFE=0 -I../../build \\
       -o plvfs plvfs.c ../../build/sqlite3.c -lm
    python3 powerloss_oracle.py [seeds...]
    SYNC=NORMAL MODES=all,subset,torn RUNS=6 BF_PRAGMAS="..." python3 powerloss_oracle.py 1 2
"""
import os, random, subprocess, sys, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("co", os.path.join(HERE, "..", "crash_oracle.py"))
co = importlib.util.module_from_spec(spec); spec.loader.exec_module(co)

PLVFS = os.environ.get("PLVFS", os.path.join(HERE, "plvfs"))
BUF = os.environ.get("BUF", os.path.join(HERE, "..", "..", "build", "sqlite3_buf"))
STOCK = os.environ.get("STOCK", os.path.join(HERE, "..", "..", "build", "sqlite3_stock"))
WORK = os.environ.get("WORK", "/tmp")
SYNC = os.environ.get("SYNC", "FULL")
MODES = os.environ.get("MODES", "all,subset,torn").split(",")
RUNS = int(os.environ.get("RUNS", "4"))          # cut points per seed and mode
NTX = int(os.environ.get("NTX", "30"))
EXTRA = os.environ.get("BF_PRAGMAS", "")
# bf_deferred_commit=N (bounded deferred durability, plan D2): a deferred commit
# writes nothing, so "no unsynced write" at its ack is vacuous; up to N-1
# acknowledged commits may be lost on top of the sync bound.
import re as _re
_m = _re.search(r'bf_(?:deferred|group)_commit\s*=\s*(\d+)', EXTRA)
WINDOW = max(int(_m.group(1)) - 1, 0) if _m else 0


def rm(db):
    for suf in ("", "-wal", "-shm", "-journal"):
        try: os.remove(db + suf)
        except FileNotFoundError: pass


def main():
    seeds = [int(x) for x in sys.argv[1:]] or [1, 2, 3]
    total = fails = 0
    for seed in seeds:
        txs = co.gen(seed, NTX)
        created = next((i + 1 for i, t in enumerate(txs)
                        if any(s.startswith("CREATE TABLE t3") for s in t)), NTX + 1)
        tabs = lambda k: co.HASHQ_TABLES if k >= created else ["t1", "t2", "t4"]
        # stock prefix hashes
        sdb = os.path.join(WORK, f"pl_st_{seed}.db"); rm(sdb)
        sc = co.HEADER + co.SCHEMA + [".print M0", co.hash_sql(tabs(0))]
        for k, st in enumerate(txs, 1):
            sc += co.tx_sql(k, st, tabs, True)
        out = subprocess.run([STOCK, sdb], input="\n".join(sc) + "\n",
                             capture_output=True, text=True).stdout.split("\n")
        H = {int(l[1:]): out[i + 1] for i, l in enumerate(out)
             if l.startswith("M") and l[1:].isdigit()}
        hv = {v: k for k, v in H.items()}
        # the BF script, one statement per line, markers after each commit
        script = co.HEADER + [f"PRAGMA synchronous={SYNC};"] + ([EXTRA] if EXTRA else []) \
            + co.SCHEMA + [".print M0"]
        for k, st in enumerate(txs, 1):
            script += co.tx_sql(k, st, tabs, False)
        text = "\n".join(script) + "\n"
        db = os.path.join(WORK, f"pl_bf_{seed}.db")
        rm(db)
        dry = subprocess.run([PLVFS, db], input=text, capture_output=True, text=True,
                             env=dict(os.environ, PL_COUNT="1")).stdout
        nev = int([l for l in dry.split("\n") if l.startswith("EVENTS")][0].split()[1])
        r = random.Random(seed)
        for mode in MODES:
            for run in range(RUNS):
                total += 1
                cut = r.randint(1, nev)
                rm(db)
                env = dict(os.environ, PL_CUT_AT=str(cut), PL_MODE=mode,
                           PL_SEED=str(seed * 1000 + run))
                o = subprocess.run([PLVFS, db], input=text, capture_output=True,
                                   text=True, env=env).stdout.split("\n")
                seen, bound = -1, -1
                errs = []
                for l in o:
                    w0 = l.split(" ")[0]
                    if w0.startswith("M") and w0[1:].isdigit():
                        seen = int(w0[1:])
                        if l.endswith(" D"): bound = seen
                    elif l.startswith("ERR"):
                        errs.append(l)
                # recover and judge
                def state():
                    h = subprocess.run([BUF, db, co.hash_sql(co.HASHQ_TABLES
                                        if "t3" in subprocess.run([BUF, db, ".tables"],
                                        capture_output=True, text=True).stdout
                                        else ["t1", "t2", "t4"])],
                                       capture_output=True, text=True)
                    ic = subprocess.run([BUF, db, "PRAGMA integrity_check;"],
                                        capture_output=True, text=True).stdout.strip()
                    j = hv.get(h.stdout.strip())
                    if j is None and "no such table" in h.stderr:
                        j = -1                       # even the schema was lost
                    return j, ic.split("\n")[0] if ic else "noresult"
                j, ic = state()
                subprocess.run([BUF, db, "PRAGMA wal_checkpoint(TRUNCATE);"],
                               capture_output=True, text=True)
                j2, ic2 = state()
                ok = (not errs and j is not None and j >= bound - WINDOW and ic == "ok"
                      and j2 == j and ic2 == "ok")
                verdict = "PASS" if ok else "FAIL"
                if not ok: fails += 1
                print(f"{verdict} seed={seed} mode={mode} cut={cut}/{nev} acked=M{seen} "
                      f"durable>=M{bound} recovered={'prefix %d' % j if j is not None else 'NOT A COMMITTED STATE'}"
                      f" ic={ic} after-ckpt={'prefix %s' % j2} ic2={ic2}"
                      + (f" errs={errs[:2]}" if errs else ""), flush=True)
                if not ok:
                    os.system(f"cp {db} {db}.fail{total} 2>/dev/null; "
                              f"cp {db}-wal {db}.fail{total}-wal 2>/dev/null")
    print(f"\n{'ALL CLEAN' if fails == 0 else 'FAILURES'}: {total - fails}/{total} passed "
          f"(synchronous={SYNC})")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
