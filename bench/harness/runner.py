#!/usr/bin/env python3
"""Matrix runner for the BF-Tree benchmark campaign.

Owns everything the C driver deliberately does not: which runs exist, how the
memory budget is split between engines, how the machine is isolated, and what
gets recorded so a result can be reproduced or disbelieved six months later.

THE FIVE THINGS THAT MAKE THIS A *PROPER* BENCHMARK
---------------------------------------------------
1. FAIR MEMORY.  Both engines get the identical number of bytes, differing only
   in what they spend it on.  Stock puts the whole budget in the page cache;
   the BF build puts `budget - page_floor` into the record cache and keeps
   `page_floor` of page cache.  Totals match exactly.  This is the "same bytes,
   records vs pages" comparison, and it is the only one the paper's claim is
   about.

2. LARGER THAN MEMORY, ENFORCED.  Each run executes inside a transient systemd
   scope with MemoryMax and MemorySwapMax=0.  A cgroup v2 memory cap covers the
   PAGE CACHE too, so the OS cannot quietly hold the whole dataset and hide the
   difference -- which is exactly what happens on an uncapped box and is why
   earlier numbers here were not trustworthy.

3. HONEST I/O ACCOUNTING.  The driver reads its own /proc/self/io from inside
   the scope.  The previous harness used getrusage(RUSAGE_CHILDREN) in the
   parent, which reads 0 under systemd-run because the process is no longer our
   child -- a documented known-bad number.  read_bytes/write_bytes are the
   hardware-independent result; wall time is the hardware-dependent one.

4. IDENTICAL STARTING STATE.  Every dataset is loaded ONCE by the stock binary
   and kept as a read-only template.  Each run that writes gets a fresh copy,
   so no run inherits another run's tree shape, WAL, or free list.

5. REPRODUCIBILITY.  Every result line carries the full config, the build
   manifest (compiler, flags, amalgamation hash, git rev, dirty bit) and the
   machine manifest (kernel, CPU, governor, RAM, filesystem, mount options).

BTRFS NOTE.  This tree lives on btrfs, whose copy-on-write and checksums add
write amplification of their own and would confound the write experiments.  The
runner therefore creates the work directory with `chattr +C` (nodatacow) so
database files are written in place, and refuses to pretend otherwise if that
fails -- it records `nodatacow: false` in the manifest so the caveat travels
with the numbers.

Usage:
  runner.py bench/harness/configs/smoke.json
  runner.py configs/full.json --only point_read --suts bf,stock --dry-run
"""
import argparse
import itertools
import json
import os
import platform
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "bin")

# How much page cache the BF build keeps for itself.  Everything the BF build
# does still goes through SQLite's pager for interior nodes and non-BF pages, so
# zero would be a strawman; this floor is subtracted from the shared budget so
# the two engines' totals stay equal to the byte.
DEFAULT_PAGE_FLOOR = 8 << 20


# ---------------------------------------------------------------------------
# environment capture
# ---------------------------------------------------------------------------
def _read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def _cmd(args):
    try:
        return subprocess.run(args, capture_output=True, text=True,
                              timeout=10).stdout.strip()
    except Exception:
        return ""


def machine_manifest(workdir):
    cpu_model = ""
    for line in _read("/proc/cpuinfo").splitlines():
        if line.startswith("model name"):
            cpu_model = line.split(":", 1)[1].strip()
            break
    gov = _read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "?")
    mem_kb = 0
    for line in _read("/proc/meminfo").splitlines():
        if line.startswith("MemTotal:"):
            mem_kb = int(line.split()[1])
            break
    fstype = _cmd(["stat", "-f", "-c", "%T", workdir])
    mount = ""
    dev = ""
    for line in _read("/proc/mounts").splitlines():
        parts = line.split()
        if len(parts) > 3 and workdir.startswith(parts[1]) and len(parts[1]) > len(mount):
            mount, dev = parts[1], parts[0]
            mountopts = parts[3]
    return {
        "hostname": platform.node(),
        "kernel": platform.release(),
        "cpu_model": cpu_model,
        "cpu_count": os.cpu_count(),
        "scaling_governor": gov,
        "mem_total_kb": mem_kb,
        "swap_total_kb": int(next(
            (l.split()[1] for l in _read("/proc/meminfo").splitlines()
             if l.startswith("SwapTotal:")), 0)),
        "thp": _read("/sys/kernel/mm/transparent_hugepage/enabled", "?"),
        "workdir_fstype": fstype,
        "workdir_mount": mount,
        "workdir_mountopts": locals().get("mountopts", ""),
        "workdir_device": dev,
        "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }


def have_systemd_run():
    return shutil.which("systemd-run") is not None


# ---------------------------------------------------------------------------
# work directory
# ---------------------------------------------------------------------------
def prepare_workdir(path):
    """Create the work dir and try to turn off btrfs COW for files inside it.

    chattr +C only takes effect for files created AFTER the flag is set on the
    directory, which is why this must happen before any database lands here.
    """
    os.makedirs(path, exist_ok=True)
    nodatacow = False
    if _cmd(["stat", "-f", "-c", "%T", path]) == "btrfs":
        subprocess.run(["chattr", "+C", path], capture_output=True)
        flags = _cmd(["lsattr", "-d", path])
        nodatacow = "C" in flags.split()[0] if flags else False
    return nodatacow


def copy_db(src, dst):
    """Copy a database template.  Plain copy, not reflink: a reflinked file on
    btrfs would take a COW fault on the first write to every extent, which lands
    squarely in the middle of the write experiments we are trying to measure."""
    for suffix in ("", "-wal", "-shm"):
        if os.path.exists(dst + suffix):
            os.remove(dst + suffix)
    shutil.copyfile(src, dst)


def drop_caches_for(path):
    for suffix in ("", "-wal", "-shm"):
        p = path + suffix
        if not os.path.exists(p):
            continue
        fd = os.open(p, os.O_RDONLY)
        try:
            os.fsync(fd)
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(fd)


# ---------------------------------------------------------------------------
# matrix expansion
# ---------------------------------------------------------------------------
WRITE_OPS = ("update", "insert", "rmw")


def mix_has_writes(workload):
    for term in workload.split(","):
        name, _, pct = term.partition("=")
        if name.strip() in WRITE_OPS and int(pct or 0) > 0:
            return True
    return False


def expand(config, only=None, suts_override=None):
    """Expand experiments x axes x suts x repeats into a flat list of runs."""
    defaults = config.get("defaults", {})
    runs = []
    for exp in config["experiments"]:
        if only and exp["name"] not in only:
            continue
        base = dict(defaults)
        base.update({k: v for k, v in exp.items()
                     if k not in ("axes", "name", "suts")})
        axes = exp.get("axes", {})
        keys = sorted(axes)
        combos = list(itertools.product(*[axes[k] for k in keys])) or [()]
        suts = suts_override or exp.get("suts") or defaults.get("suts", ["stock", "bf"])
        for combo in combos:
            cell = dict(base)
            for k, v in zip(keys, combo):
                # An axis value may be a dict, which merges several settings
                # that must vary together (e.g. a budget and the cap that has
                # to be big enough to hold it).
                if isinstance(v, dict):
                    cell.update({kk: vv for kk, vv in v.items() if kk != "label"})
                else:
                    cell[k] = v
            for sut in suts:
                for rep in range(int(cell.get("repeats", 1))):
                    r = dict(cell)
                    r["experiment"] = exp["name"]
                    r["sut"] = sut
                    r["repeat"] = rep
                    r["axis"] = {k: (v.get("label", json.dumps(v, sort_keys=True))
                              if isinstance(v, dict) else v)
                         for k, v in zip(keys, combo)}
                    runs.append(r)
    return runs


def split_memory(run, page_floor):
    """Decide the memory split for one experiment cell.

    Returns (total, ring, page_cache).  `total` depends only on the cell, not
    on the SUT, so every engine in the cell is given exactly the same bytes --
    that is the whole point of this function existing in one place.

    The ring must be a power of two: bf_circular_buffer.c masks addresses with
    capacity-1, and so does the reference (circular_buffer/mod.rs asserts
    is_power_of_two).  That constraint is faithful and stays.  What does NOT
    have to stay is what we did with the remainder.

    Rounding the ring DOWN inside a fixed budget kept the totals equal and
    quietly halved the thing under test: budget 256M with an 8M floor asks for
    a 248M ring, rounds down to 128M, and hands the other 128M to the page
    cache.  The 2026-09-16 steady campaign ran its entire larger-than-memory
    arm that way -- `bf.buffer_size` reads 134217728 in every row -- so the
    record cache was measured at half its configured size, with the surplus
    given to the component Bf-Tree exists to replace.  Rounding UP inside the
    budget is worse: it gave BF 12.5% more memory than stock.

    "match" (the default) rounds the ring UP and raises the TOTAL to match, so
    `budget_bytes` is a floor rather than a ceiling: every SUT gets
    ring + page_floor.  The ring is what the config asked for, and the totals
    are still exactly equal.  "down" and "up" reproduce the two older
    behaviours for comparison with results recorded before this.
    """
    budget = parse_size(run["budget_bytes"])
    floor = parse_size(run.get("bf_page_cache_floor", page_floor))
    floor = min(floor, budget // 2)
    ring = budget - floor
    mode = run.get("bf_ring_round", "match")

    if mode == "match":
        pow2 = 1
        while pow2 < ring:
            pow2 *= 2
        return pow2 + floor, pow2, floor
    if mode == "down":
        pow2 = 1
        while pow2 * 2 <= ring:
            pow2 *= 2
        return budget, pow2, budget - pow2
    if mode == "up":
        pow2 = 1
        while pow2 < ring:
            pow2 *= 2
        return pow2 + floor, pow2, floor
    raise SystemExit("unknown bf_ring_round %r (match|down|up)" % mode)


def resolve_memmax(run):
    """Cap for one run, resolving the "auto" keyword.

    A fixed cap across a budget sweep is a confound: at budget=512M under a 1G
    cap the engine owns half the cgroup and the OS page cache is squeezed out,
    so ops/s falls as the budget *rises* (visible in the 2026-08-25 point_read
    table, both SUTs).  "auto" keeps the engine at a constant fraction of the
    cap, so the axis varies engine cache size and nothing else.

    It scales with the EFFECTIVE total from split_memory(), not with the
    configured budget_bytes, so a cell whose ring rounded up does not quietly
    lose the headroom the cap was meant to guarantee.
    """
    mm = run.get("memmax")
    if mm != "auto":
        return mm
    return str(run["mem_split"]["total"] * 2 + (256 << 20))


def parse_size(v):
    if isinstance(v, int):
        return v
    v = str(v).strip()
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    if v and v[-1].upper() in mult:
        return int(float(v[:-1]) * mult[v[-1].upper()])
    return int(v)


# ---------------------------------------------------------------------------
# run construction
# ---------------------------------------------------------------------------
def build_argv(run, dbpath, jsonpath, page_floor):
    """Turn one expanded run into a bfbench command line.

    The memory split lives here and nowhere else, so there is exactly one place
    to check that the two engines got the same total.
    """
    sut = run["sut"]
    binary = os.path.join(BIN, "bfbench_" + sut)
    split = run["mem_split"]
    budget = split["total"]

    argv = [binary, "run",
            "--db", dbpath,
            "--records", str(run["records"]),
            "--value-len", str(run.get("value_len", 100)),
            "--key-spacing", str(run.get("key_spacing", 16)),
            "--workload", run["workload"],
            "--dist", str(run.get("dist", "zipf")),
            "--theta", str(run.get("theta", 0.9)),
            "--scan-len", str(run.get("scan_len", 32)),
            "--synchronous", str(run.get("synchronous", "normal")),
            "--journal", str(run.get("journal", "wal")),
            "--ops-per-txn", str(run.get("ops_per_txn", 1)),
            "--seed", str(run.get("seed", 7) + run["repeat"]),
            "--sut", sut,
            "--label", run["experiment"],
            "--json", jsonpath]

    if run.get("seconds"):
        argv += ["--seconds", str(run["seconds"])]
    if run.get("ops"):
        argv += ["--ops", str(run["ops"])]
    if run.get("warmup_seconds"):
        argv += ["--warmup-seconds", str(run["warmup_seconds"])]
    if run.get("warmup_ops"):
        argv += ["--warmup-ops", str(run["warmup_ops"])]
    if run.get("autocheckpoint") is not None:
        argv += ["--autocheckpoint", str(run["autocheckpoint"])]
    if run.get("mmap") is not None:
        argv += ["--mmap", str(run["mmap"])]
    if run.get("read_txn"):
        argv += ["--read-txn"]
    if run.get("drop_cache", True):
        argv += ["--drop-cache"]

    # --- the memory split: identical totals, different spending -------------
    if sut == "stock":
        argv += ["--page-cache-bytes", str(budget)]
    elif sut == "bf_off":
        # BF compiled in but switched off: same page-cache budget as stock, so
        # the difference against stock is pure fork overhead.
        argv += ["--bf-cache", "off", "--page-cache-bytes", str(budget)]
    else:
        # The floor is per-run, not global: it is a legitimate axis.  Until the
        # bf_cache.c page-hash fix it could not be swept -- a BF build with a
        # real page cache hit a fixed 256-bucket hash and ran ~13x slower than
        # pcache1 -- so every BF run was pinned at the 8 MiB floor and "equal
        # budget" quietly meant "stock gets a page cache, BF does not".
        #
        # The ring/page-cache arithmetic and its history live in
        # split_memory(); this branch only spends what it was handed.
        argv += ["--bf-cache-bytes", str(split["ring"]),
                 "--page-cache-bytes", str(split["page_cache"])]
        if run.get("group_commit") is not None:
            argv += ["--group-commit", str(run["group_commit"])]
        if run.get("promotion") is not None:
            argv += ["--promotion", str(run["promotion"])]
    return argv


def wrap_scope(argv, memmax, name):
    if not memmax:
        return argv
    return ["systemd-run", "--user", "--scope", "--quiet",
            "--unit", name,
            "-p", "MemoryMax=%s" % memmax,
            "-p", "MemorySwapMax=0",
            "--"] + argv


# ---------------------------------------------------------------------------
# dataset management
# ---------------------------------------------------------------------------
def ensure_dataset(name, spec, datadir, force=False):
    """Load a dataset template once, with the STOCK binary.

    Using stock for the load matters: it guarantees the base tree every SUT sees
    is byte-identical and in plain SQLite format, so nothing about the starting
    state can favour one engine.
    """
    path = os.path.join(datadir, name + ".db")
    meta_path = path + ".meta.json"
    meta = {"records": spec["records"],
            "value_len": spec.get("value_len", 100),
            "key_spacing": spec.get("key_spacing", 16)}
    if not force and os.path.exists(path) and os.path.exists(meta_path):
        try:
            if json.load(open(meta_path)) == meta:
                print("  dataset %-10s reuse  %.2f GiB" %
                      (name, os.path.getsize(path) / (1 << 30)))
                return path
        except Exception:
            pass
    print("  dataset %-10s loading %d rows ..." % (name, spec["records"]))
    argv = [os.path.join(BIN, "bfbench_stock"), "load",
            "--db", path,
            "--records", str(spec["records"]),
            "--value-len", str(meta["value_len"]),
            "--key-spacing", str(meta["key_spacing"])]
    subprocess.run(argv, check=True)
    json.dump(meta, open(meta_path, "w"))
    os.chmod(path, 0o444)          # template is read-only: nothing may mutate it
    return path


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config")
    ap.add_argument("--out", default=None, help="results directory")
    ap.add_argument("--only", default=None,
                    help="comma-separated experiment names to run")
    ap.add_argument("--suts", default=None, help="override the SUT list")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--reload", action="store_true", help="rebuild datasets")
    ap.add_argument("--work", default=None, help="work directory for db copies")
    ap.add_argument("--continue-on-error", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="run a strict config despite a dirty tree or a "
                         "non-performance cpu governor")
    args = ap.parse_args()

    config = json.load(open(args.config))
    name = config.get("name", os.path.basename(args.config).split(".")[0])
    workdir = args.work or os.path.join(HERE, "work")
    datadir = os.path.join(workdir, "datasets")
    outdir = args.out or os.path.join(HERE, "results", name)

    only = set(args.only.split(",")) if args.only else None
    suts_override = args.suts.split(",") if args.suts else None
    runs = expand(config, only, suts_override)

    page_floor = parse_size(config.get("defaults", {})
                            .get("bf_page_cache_floor", DEFAULT_PAGE_FLOOR))

    # One split per cell, stamped before anything reads it, so build_argv,
    # resolve_memmax, the preflight check and the recorded row cannot disagree
    # about how many bytes this cell was given.
    for r in runs:
        total, ring, pcache = split_memory(r, page_floor)
        r["mem_split"] = {"total": total, "ring": ring, "page_cache": pcache,
                          "mode": r.get("bf_ring_round", "match")}

    print("config: %s   runs: %d" % (args.config, len(runs)))
    est = sum(float(r.get("seconds", 0)) + float(r.get("warmup_seconds", 0))
              for r in runs)
    print("estimated measured+warmup time: %.0f min (plus load, copies, zipf setup)"
          % (est / 60.0))

    seen = []
    for r in runs:
        k = (r["budget_bytes"], r["mem_split"]["total"], r["mem_split"]["ring"],
             r["mem_split"]["page_cache"], r["mem_split"]["mode"])
        if k not in seen:
            seen.append(k)
    print("memory split (identical total for every SUT in a cell):")
    for budget, total, ring, pcache, mode in seen:
        print("  budget %-6s -> total %6.1f MiB = ring %6.1f + page cache %5.1f"
              "   [%s]" % (budget, total / (1 << 20), ring / (1 << 20),
                           pcache / (1 << 20), mode))

    if args.dry_run:
        for r in runs[:60]:
            print("  %-16s %-8s %-22s %s" %
                  (r["experiment"], r["sut"], r["workload"], r.get("axis")))
        if len(runs) > 60:
            print("  ... %d more" % (len(runs) - 60))
        return 0

    # --- preflight ---------------------------------------------------------
    for r in runs:
        b = os.path.join(BIN, "bfbench_" + r["sut"])
        if not os.path.exists(b):
            sys.exit("missing SUT binary %s -- run build_suts.sh --all" % b)
    if any(resolve_memmax(r) for r in runs) and not have_systemd_run():
        sys.exit("systemd-run not found, but the config asks for memmax caps")

    nodatacow = prepare_workdir(workdir)
    os.makedirs(datadir, exist_ok=True)
    os.makedirs(outdir, exist_ok=True)
    if not nodatacow:
        print("WARNING: work dir is not nodatacow; btrfs COW will inflate "
              "write_bytes. Recorded in the manifest.")

    mach = machine_manifest(workdir)
    mach["nodatacow"] = nodatacow
    try:
        build = json.load(open(os.path.join(BIN, "build_manifest.json")))
    except Exception:
        build = {}
    manifest = {"config_name": name, "config": config,
                "machine": mach, "build": build,
                "bf_page_cache_floor": page_floor}
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    # --- reproducibility gate ---------------------------------------------
    # A campaign whose numbers go into a document must be traceable to a commit
    # and must not carry avoidable frequency noise.  Configs opt in with
    # "strict": true; --force runs anyway.
    strict = bool(config.get("strict")) and not args.force
    problems = []
    if build.get("git_dirty"):
        problems.append(
            "the working tree is dirty, so '%s' does not identify the code "
            "under test (commit first, or rerun build_suts.sh after "
            "committing)" % (build.get("git_rev") or "?")[:12])
    gov = mach["scaling_governor"]
    if gov not in ("performance", "?"):
        problems.append(
            "cpu governor is '%s'; wall-clock numbers carry frequency-scaling "
            "noise (sudo cpupower frequency-set -g performance)" % gov)
    for msg in problems:
        print("%s: %s" % ("ERROR" if strict else "WARNING", msg))
    if strict and problems:
        sys.exit("refusing to run a strict config; pass --force to override")

    # --- datasets ----------------------------------------------------------
    print("datasets:")
    templates = {}
    needed = {r.get("dataset", "default") for r in runs}
    for dsname in sorted(needed):
        spec = config["datasets"][dsname]
        templates[dsname] = ensure_dataset(dsname, spec, datadir, args.reload)
        for r in runs:
            if r.get("dataset", "default") == dsname:
                r.setdefault("records", spec["records"])
                r.setdefault("value_len", spec.get("value_len", 100))
                r.setdefault("key_spacing", spec.get("key_spacing", 16))

    # Sanity: the cap must leave room for the budget plus the process itself.
    for r in runs:
        mm = resolve_memmax(r)
        if mm:
            cap = parse_size(mm)
            budget = r["mem_split"]["total"]
            # Below 2x the engine budget there is no room left for the OS page
            # cache, and the run measures cgroup reclaim rather than the engine.
            if cap < budget * 2 + (128 << 20):
                print("WARNING: %s/%s memmax=%s leaves little room outside the "
                      "%s engine budget; the OS page cache is squeezed and "
                      "throughput may fall as the budget rises. Use "
                      "\"memmax\": \"auto\" to scale the cap with the budget."
                      % (r["experiment"], r["sut"], mm, r["budget_bytes"]))
                break

    # --- run ---------------------------------------------------------------
    results_path = os.path.join(outdir, "results.jsonl")
    fout = open(results_path, "a")
    dbpath = os.path.join(workdir, "bench.db")
    jsonpath = os.path.join(workdir, "run.json")
    t_all = time.time()
    n_fail = 0

    cur_dataset = None      # which template work/bench.db currently holds
    for i, r in enumerate(runs, 1):
        dsname = r.get("dataset", "default")
        # A read-only run may reuse the existing copy ONLY if that copy came
        # from the same template.  Without this the leftover bench.db from an
        # earlier campaign (or from the previous experiment's dataset) is read
        # with the wrong key space: every lookup misses, the run is fast, and
        # the result is garbage that looks like a win.
        fresh = mix_has_writes(r["workload"]) or dsname != cur_dataset
        tag = "%s/%s/%s/rep%d" % (r["experiment"], r["sut"],
                                  ",".join("%s=%s" % kv for kv in
                                           sorted(r["axis"].items())) or "-",
                                  r["repeat"])
        print("[%3d/%3d] %s" % (i, len(runs), tag), flush=True)

        # Every write run starts from the pristine template.  Read-only runs
        # reuse the copy but are checked afterwards for accidental mutation.
        if fresh or not os.path.exists(dbpath):
            t0 = time.time()
            copy_db(templates[dsname], dbpath)
            os.chmod(dbpath, 0o644)
            cur_dataset = dsname
            print("          fresh copy of %s (%.1fs)"
                  % (dsname, time.time() - t0))
        else:
            for suffix in ("-wal", "-shm"):
                if os.path.exists(dbpath + suffix):
                    os.remove(dbpath + suffix)
        size_before = os.path.getsize(dbpath)

        if os.path.exists(jsonpath):
            os.remove(jsonpath)
        drop_caches_for(dbpath)

        argv = build_argv(r, dbpath, jsonpath, page_floor)
        unit = "bfbench-%d-%d" % (os.getpid(), i)
        full = wrap_scope(argv, resolve_memmax(r), unit)

        t0 = time.time()
        proc = subprocess.run(full, capture_output=True, text=True)
        wall = time.time() - t0
        sys.stderr.write(proc.stderr)

        record = dict(r)
        record["memmax_resolved"] = resolve_memmax(r)
        record["wall_s"] = wall
        record["returncode"] = proc.returncode
        record["argv"] = argv
        if os.path.exists(jsonpath):
            try:
                record["run"] = json.load(open(jsonpath))
            except Exception as e:
                record["error"] = "unparsable driver json: %s" % e
        else:
            record["error"] = "driver produced no json (killed? OOM?)"

        # The engine must actually hold the ring it was handed.  Nothing in
        # this harness ever compared the request against what the build
        # reports, which is how an entire campaign ran at half the configured
        # record-cache size without a single warning.  Check it every run.
        if record.get("run") and r["sut"] not in ("stock", "bf_off"):
            got = ((record["run"].get("result") or {}).get("bf") or {}) \
                    .get("buffer_size")
            want = r["mem_split"]["ring"]
            if got is not None and got != want:
                record["ring_mismatch"] = {"asked": want, "got": got}
                print("          ERROR: ring mismatch -- asked %.1f MiB, "
                      "engine reports %.1f MiB"
                      % (want / (1 << 20), got / (1 << 20)))
                if strict:
                    fout.write(json.dumps(record) + "\n")
                    fout.flush()
                    sys.exit("refusing to continue: the SUT is not running the "
                             "memory budget it was given")

        if proc.returncode != 0:
            n_fail += 1
            record.setdefault("error", "exit %d" % proc.returncode)
            record["stderr_tail"] = proc.stderr[-2000:]
            print("          FAILED: %s" % record["error"])
            if not args.continue_on_error and proc.returncode not in (3,):
                fout.write(json.dumps(record) + "\n")
                fout.flush()
                sys.exit("aborting; rerun with --continue-on-error to skip")

        if mix_has_writes(r["workload"]):
            cur_dataset = None          # mutated; the next run needs a copy

        if not fresh and os.path.getsize(dbpath) != size_before:
            record["warning"] = "read-only run changed the db file"
            print("          WARNING: read-only run mutated the database")

        fout.write(json.dumps(record) + "\n")
        fout.flush()

    fout.close()
    print("\n%d runs in %.1f min, %d failed -> %s"
          % (len(runs), (time.time() - t_all) / 60.0, n_fail, results_path))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
