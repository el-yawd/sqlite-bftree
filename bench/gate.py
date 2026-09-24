#!/usr/bin/env python3
"""The correctness gate, in one command, in parallel.

    python3 bench/gate.py            # quick tier: the per-change gate (~1-2 min)
    python3 bench/gate.py --full     # the pre-commit gate (a few minutes)

Every checker in bench/ runs as a task on a pool sized to the machine, against a
SNAPSHOT of the build/ binaries copied into a private tmpfs work dir -- so
rebuilding build/, or editing bench/, while a gate runs cannot change what the
gate tests.  (The old sequential scripts took about two hours for the full set,
and forbade touching build/ or bench/ meanwhile.)  The shell scripts remain the
reference definition of each suite; bench/difftest.py re-implements the
differential ones exactly, in parallel.

Tasks, per tier (quick = fewer seeds):
  codec       test/bf/wal_codec_test.c
  difftest    stress + stress_buf (base, ring, group8, promo100) + xtable
  repro       torn_tail_repro, recover_repro (release + SQLITE_DEBUG), ckpt_repro
  crash       crash_oracle.py sharded by seed: default, cycling ring, group 8,
              promotion 100, SQLITE_DEBUG
  powerloss   powerloss_oracle.py: FULL, NORMAL, FULL+group 8, FULL+ring
  ring        ring_repro.sh (full tier only: needs the harness dataset)

Needs build/{sqlite3, sqlite3_buf, sqlite3_stock, sqlite3_dbg, sqlite3.c}; the
power-loss driver is compiled into build/plvfs when sqlite3.c is newer than it.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor, as_completed

BENCH = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BENCH)
BUILD = os.path.join(ROOT, "build")


def sh(cmd, env=None, cwd=None):
    t = time.time()
    p = subprocess.run(cmd, shell=True, capture_output=True, text=True, env=env, cwd=cwd)
    return p.returncode, (p.stdout + p.stderr), time.time() - t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--keep", action="store_true", help="keep the work dir")
    a = ap.parse_args()
    full = a.full

    work = tempfile.mkdtemp(prefix="bfgate.", dir="/tmp")
    bins = {}
    for name in ("sqlite3", "sqlite3_buf", "sqlite3_stock", "sqlite3_dbg"):
        src = os.path.join(BUILD, name)
        if not os.access(src, os.X_OK):
            sys.exit(f"MISSING: {src} (see CLAUDE.md 'Build & test')")
        dst = os.path.join(work, name)
        shutil.copy2(src, dst)
        bins[name] = dst
    # power-loss driver: rebuild only when the amalgamation changed
    plvfs = os.path.join(BUILD, "plvfs")
    amal = os.path.join(BUILD, "sqlite3.c")
    if not os.path.exists(plvfs) or os.path.getmtime(plvfs) < os.path.getmtime(amal):
        rc, out, _ = sh(f"cc -O1 -DSQLITE_BF_INSERT_BUFFERING -DSQLITE_THREADSAFE=0 "
                        f"-I{BUILD} -o {plvfs} {BENCH}/powerloss/plvfs.c {amal} -lm")
        if rc: sys.exit("plvfs build failed:\n" + out)
    shutil.copy2(plvfs, os.path.join(work, "plvfs"))
    bins["plvfs"] = os.path.join(work, "plvfs")

    B, ST, DBG = bins["sqlite3_buf"], bins["sqlite3_stock"], bins["sqlite3_dbg"]
    tasks = []   # (name, shell command)

    def add(name, cmd): tasks.append((name, cmd))

    add("codec", f"cc -O2 -I{ROOT}/src -o {work}/codec {ROOT}/test/bf/wal_codec_test.c "
                 f"&& {work}/codec")
    # difftest is the long pole: give it every core.  The other tasks finish in
    # ~30 s and briefly oversubscribe; measured, half the machine sat idle for
    # three minutes when difftest had only half of it.
    dj = a.jobs
    add("difftest", f"python3 {BENCH}/difftest.py {'' if full else '--quick'} --jobs {dj} "
                    f"--buf {B} --stock {ST} --bf-shell {bins['sqlite3']} "
                    f"--work {work}/diff")
    add("repro:torn_tail", f"sh {BENCH}/torn_tail_repro.sh {B} && "
                           f"sh {BENCH}/torn_tail_repro.sh {DBG}")
    add("repro:recover", f"sh {BENCH}/recover_repro.sh {B} {2000 if full else 250} && "
                         f"sh {BENCH}/recover_repro.sh {DBG} 250")
    add("repro:ckpt", f"sh {BENCH}/ckpt_repro.sh {B}")

    # crash oracle, sharded: (label, env extras, seeds)
    if full:
        crash = [("default", "", range(1, 41)), ("ring", 'BF_PRAGMAS="PRAGMA bf_cache_size=262144;"', range(1, 31)),
                 ("group8", "BF_GROUP=8", range(1, 41)),
                 ("promo100", 'BF_PRAGMAS="PRAGMA bf_promotion_rate=100;"', range(41, 61)),
                 ("debug", f"BUF={DBG}", range(1, 9))]
        shard = 5
    else:
        crash = [("default", "", range(1, 5)), ("group8", "BF_GROUP=8", range(1, 4)),
                 ("ring", 'BF_PRAGMAS="PRAGMA bf_cache_size=262144;"', range(1, 3))]
        shard = 2
    for label, extra, seeds in crash:
        seeds = list(seeds)
        for i in range(0, len(seeds), shard):
            part = seeds[i:i + shard]
            d = f"{work}/crash_{label}_{part[0]}"
            add(f"crash:{label}:{part[0]}-{part[-1]}",
                f"mkdir -p {d} && BUF={B} STOCK={ST} WORK={d} {extra} "
                f"python3 {BENCH}/crash_oracle.py {' '.join(map(str, part))}")

    pl = [("FULL", "FULL", ""), ("NORMAL", "NORMAL", ""),
          ("FULL+group8", "FULL", "PRAGMA bf_deferred_commit=8;"),
          ("FULL+ring", "FULL", "PRAGMA bf_cache_size=262144;")]
    for label, sync, extra in (pl if full else pl[:1] + pl[2:3]):
        for seed in (range(1, 5) if full else range(1, 2)):
            d = f"{work}/pl_{label}_{seed}"
            add(f"powerloss:{label}:{seed}",
                f"mkdir -p {d} && PLVFS={bins['plvfs']} BUF={B} STOCK={ST} WORK={d} "
                f"SYNC={sync} BF_PRAGMAS='{extra}' RUNS={5 if full else 3} "
                f"python3 {BENCH}/powerloss/powerloss_oracle.py {seed}")
    if full:
        add("ring_repro", f"sh {BENCH}/ring_repro.sh")

    print(f"gate: {'FULL' if full else 'quick'} tier, {len(tasks)} tasks, "
          f"{a.jobs} workers, snapshot in {work}", flush=True)
    t0 = time.time()
    results = []
    # One slot per task class that matters: the tasks are processes, and the
    # short ones finish while difftest (which runs its own pool) is still going.
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(sh, cmd): name for name, cmd in tasks}
        for f in as_completed(futs):
            name = futs[f]
            rc, out, dt = f.result()
            last = [l for l in out.strip().split("\n") if l.strip()][-1:] or [""]
            print(f"{'PASS' if rc == 0 else 'FAIL'}  {name:28} {dt:6.1f}s  {last[0][:90]}",
                  flush=True)
            results.append((name, rc, out))
    bad = [r for r in results if r[1] != 0]
    for name, rc, out in bad:
        print(f"\n===== {name} (exit {rc}) =====")
        print("\n".join(l for l in out.split("\n")
                        if l.startswith(("FAIL", "DIVERG", "BUG", "SETUP", "ERR", "Error"))
                        or "error" in l.lower())[:3000] or out[-3000:])
    print(f"\n{'GATE GREEN' if not bad else 'GATE RED'}: {len(results) - len(bad)}/"
          f"{len(results)} tasks passed in {time.time() - t0:.0f}s")
    if not bad and not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f"work dir kept: {work}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
