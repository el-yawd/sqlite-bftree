#!/usr/bin/env python3
"""Phase 3 read benchmark: does a RECORD cache beat a PAGE cache per byte of
memory, on a larger-than-memory, skewed point-read workload?

That is the Bf-Tree paper's central claim, and the only honest way to test it is
to make the OS page cache stop hiding the difference.  This harness therefore:

  * builds a database several times larger than the memory the run is allowed;
  * runs the read phase inside a cgroup with a hard memory cap
    (systemd-run --scope -p MemoryMax=...), which caps page cache too, so a
    cold read really costs I/O;
  * gives BOTH engines the same cache budget -- the BF build via
    `PRAGMA bf_cache_size`, stock via `PRAGMA cache_size` -- so the comparison
    is "same bytes, spent on records vs spent on pages";
  * reports wall time AND block-input counts (getrusage ru_inblock), because
    the I/O count is the hardware-independent number.

Access pattern: Zipf-skewed point reads driven through a TEMP key table joined
against the main table, so each probe is one B-tree descent and the CLI's parse
overhead stays constant instead of scaling with the read count.

Usage:
  read_bench.py build  <db> <rows> [--binary=PATH]
  read_bench.py read   <db> <n_reads> [--binary=PATH] [--cache-bytes=N]
                       [--memmax=SIZE] [--zipf=1.1] [--seed=7] [--stock]
"""
import argparse
import os
import random
import resource
import shutil
import subprocess
import sys
import tempfile
import time

ROW_TEXT_BYTES = 96          # payload per row (hex text)


def run_cli(binary, db, sql, memmax=None):
    """Run sql through the CLI, optionally inside a memory-capped scope.

    Returns (elapsed_seconds, block_inputs, stdout).
    """
    cmd = [binary, db]
    if memmax:
        # --scope runs it as a transient cgroup; MemoryMax caps page cache too.
        cmd = ["systemd-run", "--user", "--scope", "--quiet",
               "-p", "MemoryMax=%s" % memmax,
               "-p", "MemorySwapMax=0"] + cmd
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.monotonic()
    p = subprocess.run(cmd, input=sql, capture_output=True, text=True)
    elapsed = time.monotonic() - t0
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if p.returncode != 0:
        sys.stderr.write(p.stdout + p.stderr)
        raise SystemExit("CLI failed (%d): %s" % (p.returncode, binary))
    return elapsed, after.ru_inblock - before.ru_inblock, p.stdout


def build(args):
    """Create a database of `rows` rows.  Rowids are spaced by 1000 so a later
    write phase can insert between them without splitting every leaf."""
    if os.path.exists(args.db):
        os.remove(args.db)
    for suffix in ("-wal", "-shm"):
        if os.path.exists(args.db + suffix):
            os.remove(args.db + suffix)
    sql = [
        "PRAGMA journal_mode=off;",       # bulk load: no journal, no WAL
        "PRAGMA synchronous=off;",
        "PRAGMA cache_size=-262144;",     # 256 MB while loading
        "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);",
    ]
    # Load in chunks so the temp b-tree for generate_series stays bounded.
    chunk = 1_000_000
    done = 0
    while done < args.rows:
        n = min(chunk, args.rows - done)
        sql.append(
            "INSERT INTO t SELECT (value+%d)*1000, hex(randomblob(%d)) "
            "FROM generate_series(1,%d);" % (done, ROW_TEXT_BYTES // 2, n))
        done += n
    sql.append("PRAGMA integrity_check;")
    elapsed, blocks, out = run_cli(args.binary, args.db, "\n".join(sql))
    size = os.path.getsize(args.db)
    print("built %s: %d rows, %.2f GiB, %.1fs" %
          (args.db, args.rows, size / (1 << 30), elapsed))
    if "ok" not in out:
        print("integrity_check said: %s" % out.strip()[:200])


def zipf_keys(n_reads, n_rows, s, seed):
    """Zipf-ish skewed key sample over the row space (rowids are k*1000)."""
    rng = random.Random(seed)
    # Inverse-CDF sampling on a bounded Zipf: rank r has weight 1/r^s.
    # Precompute the normalising tail so sampling is O(1) per draw.
    ranks = min(n_rows, 1_000_000)
    weights = [1.0 / ((i + 1) ** s) for i in range(ranks)]
    total = sum(weights)
    cum = []
    acc = 0.0
    for w in weights:
        acc += w
        cum.append(acc / total)
    import bisect
    keys = []
    for _ in range(n_reads):
        u = rng.random()
        r = bisect.bisect_left(cum, u)
        # Map rank -> a rowid, shuffled by a cheap mixing step so the hot set is
        # scattered across the file rather than being the first N pages.
        row = (r * 2654435761) % n_rows + 1
        keys.append(row * 1000)
    return keys


def read(args):
    n_rows = int(args.rows) if args.rows else None
    if n_rows is None:
        _, _, out = run_cli(args.binary, args.db, "SELECT count(*) FROM t;")
        n_rows = int(out.strip().splitlines()[-1])
    keys = zipf_keys(args.n_reads, n_rows, args.zipf, args.seed)

    sql = ["PRAGMA temp_store=MEMORY;"]
    if args.stock:
        # Stock: spend the whole budget on the page cache (negative = KiB).
        sql.append("PRAGMA cache_size=-%d;" % (args.cache_bytes // 1024))
    else:
        sql.append("PRAGMA bf_cache_size=%d;" % args.cache_bytes)
        sql.append("PRAGMA bf_promotion_rate=100;")
        sql.append("PRAGMA cache_size=-2048;")   # small page cache: BF holds the budget
    sql.append("CREATE TEMP TABLE k(id INTEGER);")
    # One multi-row INSERT keeps parse cost constant instead of per-read.
    step = 5000
    for i in range(0, len(keys), step):
        vals = ",".join("(%d)" % k for k in keys[i:i + step])
        sql.append("INSERT INTO k VALUES %s;" % vals)
    sql.append("SELECT 'BEGIN-READS';")
    sql.append("SELECT count(*), sum(length(t.v)) FROM k JOIN t ON t.id=k.id;")
    sql.append("SELECT 'END-READS';")
    if not args.stock:
        sql.append("PRAGMA bf_cache_stats;")

    elapsed, blocks, out = run_cli(args.binary, args.db, "\n".join(sql),
                                   memmax=args.memmax)
    hits = misses = 0
    for line in out.splitlines():
        if line.startswith("mini_page_hits|"):
            hits = int(line.split("|")[1])
        elif line.startswith("mini_page_misses|"):
            misses = int(line.split("|")[1])
    tag = "stock" if args.stock else "bf"
    print("%-6s reads=%d  budget=%dMiB  memmax=%s  %.2fs  %.0f reads/s  "
          "block_inputs=%d%s" %
          (tag, args.n_reads, args.cache_bytes >> 20, args.memmax or "-",
           elapsed, args.n_reads / elapsed if elapsed else 0, blocks,
           ("  hit_rate=%.1f%%" % (100.0 * hits / (hits + misses))
            if (hits + misses) else "")))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build")
    b.add_argument("db")
    b.add_argument("rows", type=int)
    b.add_argument("--binary", default="../build/sqlite3")

    r = sub.add_parser("read")
    r.add_argument("db")
    r.add_argument("n_reads", type=int)
    r.add_argument("--binary", default="../build/sqlite3")
    r.add_argument("--cache-bytes", type=int, default=64 << 20)
    r.add_argument("--memmax", default=None,
                   help="hard cgroup memory cap, e.g. 512M (caps page cache)")
    r.add_argument("--zipf", type=float, default=1.1)
    r.add_argument("--seed", type=int, default=7)
    r.add_argument("--rows", type=int, default=None)
    r.add_argument("--stock", action="store_true")

    args = ap.parse_args()
    if args.cmd == "build":
        build(args)
    else:
        read(args)


if __name__ == "__main__":
    sys.exit(main())
