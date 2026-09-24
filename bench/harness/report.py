#!/usr/bin/env python3
"""Turn results.jsonl into RESULTS.md.

Aggregation rules, chosen so the tables cannot flatter us:

* MEDIAN over repeats, not mean -- one page-cache-cold outlier should not move
  the headline, and one lucky run should not either.
* Spread is reported as (max-min)/median.  A ratio quoted next to a 40% spread
  is not a result, and the table says so instead of hiding it.
* The bf/stock column is a RATIO ORIENTED SO THAT >1 MEANS BF IS BETTER, for
  every metric: throughput and ops/s divide bf by stock, latency and bytes
  divide stock by bf.  Mixing orientations across tables is how benchmark
  reports accidentally claim wins.
* Any run with read_errors or read_misses is EXCLUDED from the aggregate and
  listed in a Correctness section.  A fast wrong answer is not a datapoint.

Usage:
  report.py bench/harness/results/full            # writes RESULTS.md there
  report.py results/full --out RESULTS.md --csv results.csv
"""
import argparse
import json
import os
import statistics
import sys
from collections import defaultdict


def median(xs):
    return statistics.median(xs) if xs else 0.0


def spread(xs):
    if len(xs) < 2:
        return 0.0
    m = median(xs)
    return (max(xs) - min(xs)) / m if m else 0.0


def load(resdir):
    path = os.path.join(resdir, "results.jsonl")
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    manifest = {}
    mpath = os.path.join(resdir, "manifest.json")
    if os.path.exists(mpath):
        manifest = json.load(open(mpath))
    return rows, manifest


def axis_key(r):
    ax = r.get("axis") or {}
    if not ax:
        return "-"
    return " ".join("%s=%s" % (k, ax[k]) for k in sorted(ax))


def metrics(r):
    """Pull the numbers we report out of one driver result."""
    run = r.get("run") or {}
    res = run.get("result") or {}
    lat = res.get("latency") or {}
    io = res.get("io") or {}
    files = res.get("files") or {}
    bf = res.get("bf") or {}
    cpu = res.get("cpu") or {}

    # The latency we quote is the dominant op of the mix, so a 95/5 workload is
    # reported by its reads and a scan workload by its scans.
    dominant = None
    best = -1
    for op, h in lat.items():
        if op == "commit":
            continue
        if h.get("count", 0) > best:
            best, dominant = h.get("count", 0), op
    h = lat.get(dominant, {}) if dominant else {}
    commits = res.get("txns", 0) or 0
    wal_written = max(0, files.get("wal_bytes_after", 0)
                      - files.get("wal_bytes_before", 0))

    return {
        "ops_s": res.get("throughput_ops_s", 0.0),
        "txn_s": res.get("txn_throughput_s", 0.0),
        "dominant_op": dominant or "-",
        "p50_us": h.get("p50_ns", 0) / 1000.0,
        "p99_us": h.get("p99_ns", 0) / 1000.0,
        "p999_us": h.get("p999_ns", 0) / 1000.0,
        "commit_p50_us": (lat.get("commit", {}) or {}).get("p50_ns", 0) / 1000.0,
        "commit_p99_us": (lat.get("commit", {}) or {}).get("p99_ns", 0) / 1000.0,
        "read_MiB": io.get("read_bytes", 0) / (1 << 20),
        "write_MiB": io.get("write_bytes", 0) / (1 << 20),
        "read_bytes_per_op": (io.get("read_bytes", 0) / res["ops"]
                              if res.get("ops") else 0.0),
        "write_bytes_per_commit": (wal_written / commits) if commits else 0.0,
        "cpu_s": cpu.get("user_s", 0.0) + cpu.get("sys_s", 0.0),
        "maxrss_MiB": cpu.get("maxrss_kb", 0) / 1024.0,
        # Record-cache hit rate.  Before the bf_cache.c counter split this
        # shared its counters with the pcache2 xFetch path, so it reported the
        # PAGE hit rate -- which is why it sat at a flat 56.7% across an 8x
        # sweep of bf_cache_size in the 2026-08-25 campaign.  Results produced
        # before the split have no page_cache_hits key; those files are marked
        # unreliable rather than silently re-plotted.
        # Since 2026-09-24 (H1b) the hit rate is PER SEEK: seek_served (the
        # descent shortcut served the row; the leaf was not read) over all
        # read-cursor rowid seeks.  mini_page_hits/misses count LOOKUPS -- a
        # missed read makes ~1.8 of them, a hit ~1 -- so the old ratio
        # understated every hit rate (h1b: 32.7% by lookups, 46.8% per read).
        # Older results have no seek_* keys and fall back to the lookup ratio,
        # reported as bf_lookup_hit_rate; do not compare the two.
        "bf_hit_rate": (100.0 * bf.get("seek_served", 0)
                        / max(1, bf.get("seek_served", 0) + bf.get("seek_leaf", 0))
                        if "seek_served" in bf else None),
        "bf_lookup_hit_rate": (100.0 * bf.get("mini_page_hits", 0)
                        / max(1, bf.get("mini_page_hits", 0)
                              + bf.get("mini_page_misses", 0))
                        if "mini_page_hits" in bf else None),
        "bf_page_hit_rate": (100.0 * bf.get("page_cache_hits", 0)
                             / max(1, bf.get("page_cache_hits", 0)
                                   + bf.get("page_cache_misses", 0))
                             if "page_cache_hits" in bf else None),
        "bf_counters_split": ("page_cache_hits" in bf) if bf else None,
        "bf_page_frames_per_commit": (bf.get("wal_page_frames", 0)
                                      / max(1, bf.get("wal_commits", 0))
                                      if "wal_commits" in bf else None),
        "bf_record_frames_per_commit": (bf.get("wal_record_frames", 0)
                                        / max(1, bf.get("wal_commits", 0))
                                        if "wal_commits" in bf else None),
        "bf_fallbacks": bf.get("insert_fallbacks"),
        "bf_buffered": bf.get("buffered_inserts"),
        # Share of write-back inserts that actually stayed in a mini-page.
        # This is the number the 2026-09-07 size-class fixes were about: the
        # old code upgraded one class per write, so anything over ~96 B fell
        # back to the base-page path and the record-granular win vanished.
        "bf_buffered_pct": (100.0 * bf.get("buffered_inserts", 0)
                            / max(1, bf.get("buffered_inserts", 0)
                                  + bf.get("insert_fallbacks", 0))
                            if "buffered_inserts" in bf else None),
        # cb_capacity is what the ring ACTUALLY is; buffer_size only echoes
        # the config.  They disagreed (8 MiB vs 256 MiB) until PRAGMA
        # bf_cache_size was made to resize a live cache, so print the real one.
        "bf_ring_MiB": (bf.get("cb_capacity") / (1 << 20)
                        if "cb_capacity" in bf else None),
        "bf_cached_records": bf.get("cached_records"),
        # Bytes of ring spent per record actually cached: mini-page header +
        # meta array + size-class round-up, amortised over however many records
        # share the page.  This is the number that decides every capacity
        # question, and it was computed by hand for a year.
        #
        # 2026-09-16: 250 B to cache a 116 B record, stable at 240-252 B across
        # a 16 MiB and a 256 MiB ring and both skews -- so it is a property of
        # the layout, not of pressure.  Two things move it: records per
        # mini-page (1.26 under scrambled zipf, because a lone hot record pays
        # for a whole header) and the size-class granularity (ours is a fixed
        # 64..4096 doubling; the reference derives classes from the record
        # size, tree.rs:222-250, giving a 192 B class where we take 256).
        # Stage B1 attacks the second.
        "bf_bytes_per_record": (bf.get("mini_page_bytes") / bf["cached_records"]
                                if bf.get("cached_records") else None),
        "bf_records_per_page": (bf.get("cached_records") / bf["live_mini_pages"]
                                if bf.get("live_mini_pages") else None),
        "bf_evictions": bf.get("evictions"),
        "bf_upgrades": bf.get("upgrades"),
        "read_errors": res.get("read_errors", 0) + res.get("read_misses", 0),
    }


# Run-to-run spread above this is too noisy to read a ratio from: such a cell
# is bolded in the table and counted in the "Reading these numbers" section.
# The 2026-08-25 campaign had cells at 80% and 120% spread whose ratios were
# quoted as findings; they were noise.
NOISE_THRESHOLD = 0.15

# Metrics where a SMALLER number is better, so the ratio must be inverted for
# the ">1 means BF wins" convention.
LOWER_IS_BETTER = {"p50_us", "p99_us", "p999_us", "commit_p50_us",
                   "commit_p99_us", "read_MiB", "write_MiB", "cpu_s",
                   "read_bytes_per_op", "write_bytes_per_commit",
                   "maxrss_MiB", "bf_bytes_per_record"}


def ratio(metric, bf_val, stock_val):
    if not bf_val or not stock_val:
        return None
    return (stock_val / bf_val) if metric in LOWER_IS_BETTER else (bf_val / stock_val)


def fmt(v, nd=1):
    if v is None:
        return "-"
    if isinstance(v, str):
        return v
    if v >= 100000:
        return "%.0f" % v
    return ("%%.%df" % nd) % v


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("resdir")
    ap.add_argument("--out", default=None)
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()

    rows, manifest = load(args.resdir)
    out_path = args.out or os.path.join(args.resdir, "RESULTS.md")

    bad = [r for r in rows if metrics(r)["read_errors"] > 0
           or r.get("error") or r.get("warning")]
    good = [r for r in rows if r not in bad]

    # (experiment, axis, sut) -> list of per-repeat metric dicts
    groups = defaultdict(list)
    for r in good:
        groups[(r["experiment"], axis_key(r), r["sut"])].append(metrics(r))

    L = []
    W = L.append
    W("# BF-Tree v2 benchmark results\n")

    mach = manifest.get("machine", {})
    build = manifest.get("build", {})
    W("## Setup\n")
    W("| | |")
    W("|---|---|")
    W("| machine | %s, %s, %d cores |" % (mach.get("cpu_model", "?"),
                                          mach.get("kernel", "?"),
                                          mach.get("cpu_count", 0)))
    W("| RAM | %.1f GiB (swap %.1f GiB) |"
      % (mach.get("mem_total_kb", 0) / (1 << 20),
         mach.get("swap_total_kb", 0) / (1 << 20)))
    W("| cpu governor | `%s` |" % mach.get("scaling_governor", "?"))
    W("| storage | %s on %s, opts `%s`, nodatacow=%s |"
      % (mach.get("workdir_fstype", "?"), mach.get("workdir_device", "?"),
         mach.get("workdir_mountopts", "?"), mach.get("nodatacow")))
    W("| git rev | `%s`%s |" % (build.get("git_rev", "?")[:12],
                               " **(dirty tree)**" if build.get("git_dirty") else ""))
    W("| compiler | %s |" % build.get("cc", "?"))
    W("| common flags | `%s` |" % build.get("common_flags", "?"))
    W("| amalgamation sha256 | `%s` |" % build.get("amalgamation_sha256", "?")[:16])
    cfg = manifest.get("config", {})
    for dsname, ds in (cfg.get("datasets") or {}).items():
        W("| dataset `%s` | %s rows x %s B values, key spacing %s |"
          % (dsname, "{:,}".format(ds["records"]), ds.get("value_len", 100),
             ds.get("key_spacing", 16)))
    d = cfg.get("defaults", {})
    # Show every split that actually ran.  The floor is a per-run axis now, so
    # a single default line can misdescribe the campaign.
    caps = sorted({str(r.get("memmax_resolved") or r.get("memmax"))
                   for r in rows if r.get("memmax") or r.get("memmax_resolved")})
    budgets = sorted({str(r.get("budget_bytes")) for r in rows
                      if r.get("budget_bytes")})
    floors = sorted({str(r.get("bf_page_cache_floor")) for r in rows
                     if r.get("bf_page_cache_floor")})
    W("| memory | cgroup cap %s, engine budget %s |"
      % (", ".join(caps) or "?", ", ".join(budgets) or "?"))
    W("| BF split | %s of the budget stays in the page cache, the rest is the "
      "record cache |" % (", ".join(floors) or "?"))
    W("| per run | %ss measured after %ss warmup, %s repeats, median reported |"
      % (d.get("seconds"), d.get("warmup_seconds"), d.get("repeats")))
    W("")
    W("Ratios are oriented so **>1 always means BF is better**: throughput "
      "divides bf by stock, latency and bytes divide stock by bf.\n")

    # ---- per-experiment tables --------------------------------------------
    experiments = []
    for (exp, ax, sut) in groups:
        if exp not in experiments:
            experiments.append(exp)

    csv_rows = []
    n_noisy = 0
    for exp in experiments:
        W("## %s\n" % exp)
        axes = sorted({ax for (e, ax, s) in groups if e == exp})
        suts = sorted({s for (e, ax, s) in groups if e == exp})

        has_commit = any(median([m["commit_p50_us"] for m in groups[(exp, ax, s)]])
                         for ax in axes for s in suts if (exp, ax, s) in groups)

        hdr = ["config", "sut", "ops/s", "spread", "p50 us", "p99 us", "p99.9 us"]
        if has_commit:
            hdr += ["commit p50 us", "commit p99 us", "WAL B/commit"]
        hdr += ["read MiB", "write MiB", "CPU s", "rec hit%", "pg hit%"]
        W("| " + " | ".join(hdr) + " |")
        W("|" + "---|" * len(hdr))

        for ax in axes:
            for s in suts:
                ms = groups.get((exp, ax, s))
                if not ms:
                    continue
                sp = spread([m["ops_s"] for m in ms])
                noisy = sp > NOISE_THRESHOLD
                if noisy:
                    n_noisy += 1
                row = [ax, s,
                       fmt(median([m["ops_s"] for m in ms]), 0),
                       ("**%.0f%%**" % (100 * sp)) if noisy
                       else ("%.0f%%" % (100 * sp)),
                       fmt(median([m["p50_us"] for m in ms]), 1),
                       fmt(median([m["p99_us"] for m in ms]), 1),
                       fmt(median([m["p999_us"] for m in ms]), 1)]
                if has_commit:
                    row += [fmt(median([m["commit_p50_us"] for m in ms]), 1),
                            fmt(median([m["commit_p99_us"] for m in ms]), 1),
                            fmt(median([m["write_bytes_per_commit"] for m in ms]), 0)]
                hit = [m["bf_hit_rate"] for m in ms if m["bf_hit_rate"] is not None]
                hfmt = lambda h: fmt(median(h), 1)
                if not hit:          # pre-2026-09-24 results: lookup ratio, marked
                    hit = [m["bf_lookup_hit_rate"] for m in ms
                           if m["bf_lookup_hit_rate"] is not None]
                    hfmt = lambda h: fmt(median(h), 1) + "L"
                phit = [m["bf_page_hit_rate"] for m in ms
                        if m.get("bf_page_hit_rate") is not None]
                row += [fmt(median([m["read_MiB"] for m in ms]), 1),
                        fmt(median([m["write_MiB"] for m in ms]), 1),
                        fmt(median([m["cpu_s"] for m in ms]), 1),
                        hfmt(hit) if hit else "-",
                        fmt(median(phit), 1) if phit else "-"]
                W("| " + " | ".join(row) + " |")
                csv_rows.append([exp, ax, s] + row[2:])

        # Record-cache internals, for the BF suts that report them.  These are
        # the columns that tell a win from a coincidence: a throughput change
        # with a flat ring size and a flat buffered% is not a record-cache
        # effect.  cb_capacity, cached_records and upgrades are GAUGES.
        bf_suts = [s for s in suts if s != "stock"]
        det = [(ax, s, groups[(exp, ax, s)]) for ax in axes for s in bf_suts
               if (exp, ax, s) in groups
               and any(m["bf_ring_MiB"] is not None or m["bf_buffered_pct"] is not None
                       for m in groups[(exp, ax, s)])]
        if det:
            W("")
            W("| config | sut | ring MiB | cached recs | B/record | recs/page | "
              "evictions | upgrades | buffered% | rec frames/commit | "
              "pg frames/commit |")
            W("|---|---|---|---|---|---|---|---|---|---|---|")
            for ax, s, ms in det:
                def med(metric, nd=1):
                    vs = [m[metric] for m in ms if m[metric] is not None]
                    return fmt(median(vs), nd) if vs else "-"
                W("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                    ax, s, med("bf_ring_MiB"), med("bf_cached_records", 0),
                    med("bf_bytes_per_record", 0), med("bf_records_per_page", 2),
                    med("bf_evictions", 0), med("bf_upgrades", 0),
                    med("bf_buffered_pct"), med("bf_record_frames_per_commit", 2),
                    med("bf_page_frames_per_commit", 2)))
            # A "-" in these columns can mean the counter does not exist in
            # that build rather than that it read zero.  The space gauges
            # (cb_capacity, cached_records) and the record/page hit-rate split
            # are newer than some SUTs a config may name -- bf_pre in
            # particular -- and for such a build `rec hit%` is the OLD
            # conflated counter that counted page hits too, so it is not
            # comparable with a split build's record hit rate.
            unsplit = sorted({s for ax, s, ms in det
                              if any(m["bf_counters_split"] is False for m in ms)})
            if unsplit:
                W("")
                W("> %s predate(s) the counter split: their `rec hit%%` mixes in "
                  "page hits and their gauge columns are absent, not zero. "
                  "Compare `buffered%%` and frames/commit, which exist in both."
                  % ", ".join("`%s`" % u for u in unsplit))

        # Ratio blocks.  "bf vs stock" is the paper's claim; "bf vs bf_pre" is
        # the effect of OUR last change, and it is the only trustworthy way to
        # ask that question -- identical code measured in three separate
        # campaigns gave 9913 / 11652 / 8102 ops/s (23% spread), so a before /
        # after taken from two results directories means nothing.  Both
        # baselines therefore have to be SUTs inside the same run.
        for base in ("stock", "bf_pre"):
            if "bf" not in suts or base not in suts:
                continue
            W("")
            W("| config | ops/s bf/%s | p50 %s/bf | p99 %s/bf | "
              "read bytes/op %s/bf | WAL B/commit %s/bf |"
              % (base, base, base, base, base))
            W("|---|---|---|---|---|---|")
            for ax in axes:
                b = groups.get((exp, ax, "bf"))
                st = groups.get((exp, ax, base))
                if not b or not st:
                    continue
                def rr(metric):
                    return ratio(metric,
                                 median([m[metric] for m in b]),
                                 median([m[metric] for m in st]))
                W("| %s | %s | %s | %s | %s | %s |" % (
                    ax,
                    fmt(rr("ops_s"), 2), fmt(rr("p50_us"), 2),
                    fmt(rr("p99_us"), 2), fmt(rr("read_bytes_per_op"), 2),
                    fmt(rr("write_bytes_per_commit"), 2)))
        W("")

    # ---- BF-specific write-path counters ----------------------------------
    wa = [(e, ax, groups[(e, ax, "bf")]) for (e, ax, s) in groups if s == "bf"
          and any(m["bf_page_frames_per_commit"] is not None
                  for m in groups[(e, ax, "bf")])]
    if wa:
        W("## BF write-path counters (bf only)\n")
        W("| experiment | config | page frames/commit | record frames/commit | "
          "buffered inserts | insert fallbacks |")
        W("|---|---|---|---|---|---|")
        seen = set()
        for e, ax, ms in wa:
            if (e, ax) in seen:
                continue
            seen.add((e, ax))
            W("| %s | %s | %s | %s | %s | %s |" % (
                e, ax,
                fmt(median([m["bf_page_frames_per_commit"] or 0 for m in ms]), 2),
                fmt(median([m["bf_record_frames_per_commit"] or 0 for m in ms]), 2),
                fmt(median([m["bf_buffered"] or 0 for m in ms]), 0),
                fmt(median([m["bf_fallbacks"] or 0 for m in ms]), 0)))
        W("")
        W("The Phase-2 claim holds when page frames/commit sits at the WAL "
          "format floor (~1) and insert fallbacks stay near zero.\n")

    # ---- correctness / excluded runs --------------------------------------
    W("## Correctness and excluded runs\n")
    if not bad:
        W("No run reported a read error, a driver failure, or an unexpected "
          "database mutation. All %d runs are included.\n" % len(rows))
    else:
        W("**%d of %d runs were excluded.** A run that returns wrong data is "
          "not a datapoint, so these are removed from every table above.\n"
          % (len(bad), len(rows)))
        W("| experiment | sut | config | problem |")
        W("|---|---|---|---|")
        for r in bad:
            why = r.get("error") or r.get("warning") or (
                "%d read errors/misses" % metrics(r)["read_errors"])
            W("| %s | %s | %s | %s |" % (r["experiment"], r["sut"],
                                         axis_key(r), why))
        W("")

    # ---- how to read the tables --------------------------------------------
    W("## Reading these numbers\n")
    W("- `rec hit%` is the record (mini-page) cache hit rate; `pg hit%` is the "
      "pcache2 page hit rate. They are separate counters. A high `pg hit%` "
      "next to a low `rec hit%` means the win, if any, is coming from the "
      "page cache and not from the Bf-Tree record buffer.")
    W("- `rec hit%` is PER SEEK: the share of read-cursor rowid seeks the descent "
      "shortcut served without reading the leaf (`seek_served`/`seek_leaf`). A value "
      "suffixed `L` predates those counters and is the LOOKUP ratio "
      "`mini_page_hits/(hits+misses)`, which counts a missed read ~1.8 times and "
      "reads 10-15 points low; never compare the two.")
    # Only the CURRENT build being under test invalidates the whole report; an
    # intentionally old comparison SUT (bf_pre) does not, and saying it does
    # would tell the reader to discard hit rates that are in fact fine.  Those
    # SUTs are footnoted under their own tables instead.
    unsplit_suts = sorted({r["sut"] for r in rows if r["sut"].startswith("bf")
                           and metrics(r).get("bf_counters_split") is False})
    if "bf" in unsplit_suts:
        W("- **These results predate the record/page counter split.** Any "
          "`rec hit%` above blends record hits with pcache2 `xFetch` hits and "
          "must not be quoted. Re-run against a build that reports "
          "`page_cache_hits`.")
    elif unsplit_suts:
        W("- `rec hit%%` for %s comes from a build older than the counter "
          "split, so it blends in page hits; it is not comparable with the "
          "`bf` row's record hit rate. The gauge columns are absent for those "
          "SUTs, not zero."
          % ", ".join("`%s`" % u for u in unsplit_suts))
    if n_noisy:
        W("- **%d cells have run-to-run spread above %.0f%%** (bolded). A "
          "ratio built from a bolded cell is not a finding; raise `repeats` "
          "or `seconds` for that experiment and re-run before quoting it."
          % (n_noisy, 100 * NOISE_THRESHOLD))
    else:
        W("- Every cell held run-to-run spread under %.0f%%."
          % (100 * NOISE_THRESHOLD))
    W("")

    # ---- caveats -----------------------------------------------------------
    W("## Caveats that travel with these numbers\n")
    W("- **Single threaded.** The fork keeps SQLite's single-writer "
      "serialisation, so the paper's concurrency results are a documented "
      "non-transfer and are not measured here.")
    W("- **Rowid tables only.** v1 buffers the primary B-tree of rowid tables; "
      "secondary indexes and non-rowid tables stay write-through, so a schema "
      "with indexes will see a smaller effect than these tables show.")
    if mach.get("nodatacow") is False:
        W("- **btrfs COW was ACTIVE** for the work directory, so `write MiB` "
          "includes filesystem copy-on-write and checksum traffic on top of "
          "the engine's own writes. Treat write volumes as upper bounds.")
    else:
        W("- Work directory is `nodatacow`, so btrfs copy-on-write is not "
          "inflating the write volumes.")
    if mach.get("scaling_governor") not in ("performance", "?"):
        W("- **CPU governor was `%s`, not `performance`.** Wall-clock numbers "
          "carry frequency-scaling noise; the I/O byte counts do not."
          % mach.get("scaling_governor"))
    W("- `read MiB`/`write MiB` come from `/proc/self/io`, i.e. block-layer "
      "bytes for the benchmark process, measured inside the cgroup.")
    W("")

    text = "\n".join(L)
    with open(out_path, "w") as f:
        f.write(text)
    print("wrote %s (%d runs, %d excluded)" % (out_path, len(rows), len(bad)))

    if args.csv:
        import csv
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["experiment", "config", "sut", "ops_s", "spread",
                        "p50_us", "p99_us", "p999_us", "extra..."])
            w.writerows(csv_rows)
        print("wrote %s" % args.csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
