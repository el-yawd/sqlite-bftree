#!/usr/bin/env python3
"""Parallel differential oracle: stress.sh + stress_buf.sh (+ its knob variants)
+ stress_xtable.sh in one run, on every core, on tmpfs.

Same tests, same generators, seeds, journal modes, pragma injection and
byte-for-byte .out/.dump comparison against stock as the shell scripts -- those
stay as the reference definition.  What changes is only how they are run:

  * PARALLEL.  Every (suite, variant, generator, seed, journal) case is
    independent; they run on a pool of os.cpu_count() workers.  The shell
    scripts ran one at a time on a 12-core box.
  * TMPFS.  The work dir defaults to a fresh directory under /tmp.  On this
    machine /home is btrfs-on-LUKS, where the default synchronous=FULL makes
    every commit a real fsync: 2.75x slower per test, for nothing -- a
    differential oracle compares answers, and power loss has its own oracle
    (bench/powerloss/).
  * SNAPSHOT.  The binaries are copied into the work dir first, so a rebuild of
    build/ during a run cannot change what is being tested (the old rule was
    "build/ and bench/ are frozen while a suite runs").

Usage (from anywhere):
    python3 bench/difftest.py                 # everything, full seed lists
    python3 bench/difftest.py --quick         # 3 seeds per suite: the per-change tier
    python3 bench/difftest.py --suite buf --variant ring --seeds 1,2
    python3 bench/difftest.py --jobs 6 --work /some/dir --keep
Suites: stress, buf, xtable.  Variants of buf: base, ring, group8, promo100.
Exit status 0 iff every case passed.  Failing cases keep their files.
"""
import argparse, os, shutil, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor, as_completed

BENCH = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(BENCH, "..", "build")

FULL_SEEDS = [1, 2, 3, 5, 7, 11, 13, 17, 23, 42, 99, 123, 777, 1024, 2024, 12345,
              31337, 65535]
XT_SEEDS = [1, 7, 42, 1024, 31337]
QUICK_SEEDS = [1, 42, 31337]
JOURNALS = ["delete", "wal", "memory"]
BUF_GENS = ["gen_stress", "gen_stress_rand", "gen_merge_stress", "gen_rev_stress",
            "gen_rollback_stress"]
# stress_buf.sh's knob variants: PRAGMAs inserted after the leading PRAGMA block
VARIANTS = {
    "base": [],
    "ring": ["PRAGMA bf_cache_size=262144;"],
    "group8": ["PRAGMA bf_group_commit=8;"],
    "promo100": ["PRAGMA bf_promotion_rate=100;"],
}
NOPS = int(os.environ.get("NOPS", "4000"))


def gen_sql(gen, seed, jm):
    def py(*a):
        return subprocess.run(["python3", os.path.join(BENCH, a[0])] + [str(x) for x in a[1:]],
                              capture_output=True, text=True, check=True).stdout
    head = "PRAGMA journal_mode=%s;\n" % jm
    if gen == "gen_stress":
        return py("gen_stress.py", seed, jm, NOPS)
    if gen == "gen_stress_rand":
        return head + py("gen_stress_rand.py", seed, NOPS)
    if gen == "gen_merge_stress":
        return head + py("gen_merge_stress.py", seed, 400)
    if gen == "gen_rev_stress":
        return head + py("gen_rev_stress.py", seed, 400)
    if gen == "gen_rollback_stress":
        return head + py("gen_rollback_stress.py", seed, 300, jm)
    if gen == "gen_xtable":
        return py("gen_xtable.py", seed, jm, os.environ.get("NTAB", "12"),
                  os.environ.get("ROWS", "3000"))
    raise ValueError(gen)


def inject(sql, pragmas):
    """stress_buf.sh's placement -- after the leading PRAGMA block, because a
    journal_mode change drops BF settings made before it -- but appended to the
    block's LAST LINE instead of inserted as new lines.  Same statement order;
    no line numbers shift.  That matters: stock's error messages cite script
    line numbers, and keeping them fixed is what makes stock's output identical
    across variants, so it runs once per script (stock_case)."""
    if not pragmas:
        return sql
    lines = sql.split("\n")
    last = -1
    for i, l in enumerate(lines):
        if l[:7].upper() == "PRAGMA ":
            last = i
        else:
            break
    if last < 0:
        return " ".join(pragmas) + " " + sql      # no block: prefix line 1
    lines[last] = lines[last] + " " + " ".join(pragmas)
    return "\n".join(lines)


def run_engine(b, db, sql):
    for suf in ("", "-wal", "-shm", "-journal"):
        try: os.remove(db + suf)
        except FileNotFoundError: pass
    out = subprocess.run([b, db], input=sql, capture_output=True, text=True)
    dump = subprocess.run([b, db, ".dump"], capture_output=True, text=True)
    for suf in ("", "-wal", "-shm", "-journal"):
        try: os.remove(db + suf)
        except FileNotFoundError: pass
    return (out.stdout + out.stderr, dump.stdout + dump.stderr)


def stock_case(key, bins, work):
    """Phase 1: generate the script and run STOCK once per (gen, seed, journal).
    Stock ignores the BF pragmas the variants inject (unknown pragmas are
    no-ops that print nothing), so its answer is the same for every variant --
    running it once instead of four times removes a third of all process runs."""
    gen, seed, jm = key
    sql = gen_sql(gen, seed, jm)
    return key, sql, run_engine(bins["stock"], os.path.join(work, f"st_{gen}_s{seed}_{jm}.db"), sql)


def run_case(case, bins, work, keep, base_sql, st_res):
    suite, variant, gen, seed, jm = case
    tag = f"{suite}_{variant}_{gen}_s{seed}_{jm}"
    base = os.path.join(work, tag)
    sql = inject(base_sql, VARIANTS.get(variant, []))
    bf = bins["bf_shell"] if suite == "stress" else bins["buf"]
    res = {"bf": run_engine(bf, f"{base}.bf.db", sql), "st": st_res}
    ok = res["bf"] == res["st"]
    if ok and not keep:
        for f in os.listdir(work):
            if f.startswith(tag + "."):
                os.remove(os.path.join(work, f))
    else:
        with open(base + ".sql", "w") as f: f.write(sql)
        for eng in ("bf", "st"):
            with open(f"{base}.{eng}.out", "w") as f: f.write(res[eng][0])
            with open(f"{base}.{eng}.dump", "w") as f: f.write(res[eng][1])
    return case, ok, base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--suite", default="stress,buf,xtable")
    ap.add_argument("--variant", default=",".join(VARIANTS))
    ap.add_argument("--gens", default=",".join(BUF_GENS))
    ap.add_argument("--seeds", default="")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--work", default="")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--buf", default=os.path.join(BUILD, "sqlite3_buf"))
    ap.add_argument("--stock", default=os.path.join(BUILD, "sqlite3_stock"))
    ap.add_argument("--bf-shell", default=os.path.join(BUILD, "sqlite3"))
    a = ap.parse_args()

    work = a.work or tempfile.mkdtemp(prefix="bfdiff.", dir="/tmp")
    os.makedirs(work, exist_ok=True)
    bins = {}
    for k, p in (("buf", a.buf), ("stock", a.stock), ("bf_shell", a.bf_shell)):
        if not os.access(p, os.X_OK):
            sys.exit(f"MISSING: {p}")
        dst = os.path.join(work, "bin_" + k)
        shutil.copy2(p, dst)                       # snapshot: rebuilds can't bite
        bins[k] = dst

    suites = a.suite.split(",")
    seeds = ([int(x) for x in a.seeds.split(",")] if a.seeds
             else QUICK_SEEDS if a.quick else None)
    cases = []
    if "stress" in suites:
        for s in seeds or FULL_SEEDS:
            for jm in JOURNALS:
                cases.append(("stress", "base", "gen_stress", s, jm))
    if "buf" in suites:
        for v in a.variant.split(","):
            for g in a.gens.split(","):
                for s in seeds or FULL_SEEDS:
                    for jm in JOURNALS:
                        cases.append(("buf", v, g, s, jm))
    if "xtable" in suites:
        for s in seeds or XT_SEEDS:
            for jm in JOURNALS:
                cases.append(("xtable", "base", "gen_xtable", s, jm))

    t0 = time.time()
    fails = []
    keys = sorted({(c[2], c[3], c[4]) for c in cases})
    stock = {}
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for f in as_completed([ex.submit(stock_case, k, bins, work) for k in keys]):
            k, sql, res = f.result()
            stock[k] = (sql, res)
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = [ex.submit(run_case, c, bins, work, a.keep,
                          stock[(c[2], c[3], c[4])][0], stock[(c[2], c[3], c[4])][1])
                for c in cases]
        for f in as_completed(futs):
            case, ok, base = f.result()
            if not ok:
                fails.append((case, base))
                print("FAIL  %s/%s %s seed=%s journal=%s  (%s.*)" % (case + (base,)),
                      flush=True)
    dt = time.time() - t0
    by = {}
    for c in cases:
        by.setdefault((c[0], c[1]), [0, 0])[0] += 1
    for (c, _) in fails:
        by[(c[0], c[1])][1] += 1
    for (s, v), (n, nf) in sorted(by.items()):
        print(f"  {s:7} {v:9} {n - nf}/{n} passed")
    print(f"\n{'ALL CLEAN' if not fails else 'DIVERGENCES'}: {len(cases) - len(fails)}/"
          f"{len(cases)} passed in {dt:.0f}s on {a.jobs} workers (work: {work})")
    if not fails and not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
