# BF-Tree v2 — project orientation

This is a **fresh fork of SQLite 3.52.0** that adds the **Bf-Tree** record-level cache
(VLDB'24, `microsoft/bf-tree`) with a **single record-granular physiological WAL** for
durability. It supersedes an earlier integration fork (`../sqlite`, branch
`ablation-phase0`), reusing its *knowledge*, not its code.

## Read these first (in order)
1. **`BF_TREE_V2_KNOWLEDGE.md`** — the *map*: paper durability model (§5.7), data-structure
   field tables, the 5 mini-page bugs to avoid by construction, integration edge-cases.
2. **`BF_TREE_V2_PLAN.md`** — the *route*: staged plan (Phase 0–4), testing strategy,
   critical files, verification.
3. `docs/reference/` — the prior fork's design docs (old transparent-cache design that v2
   reverses; cited by the knowledge doc).

## The one insight that drives everything
Bf-Tree's wins come from the **mini-page buffer pool, not durability** — the paper's
numbers run with WAL/fsync **off**, direct-I/O, larger-than-RAM. So: the WAL change earns
the **write** win (commit persists a small record, not a 4 KB page) *only with group-commit*;
the **read/scan** wins need no format change, just the buffer pool + a direct-I/O
larger-than-RAM benchmark. Don't expect wins on an OS-page-cache-dominated box.

## Locked design decisions
- **One** record-granular **physiological WAL** (extend SQLite's WAL frame format): BF
  rowid-table leaf mutations log as `[pgno, op, key, val]`; everything else stays page-image
  frames. No sidecar / two-log design.
- **Configurable durability**: group-commit (~1 ms, paper-faithful) *and* strict per-commit
  fsync via `PRAGMA synchronous`; measure both.
- **Single-writer** (keep `BtShared` serialization); concurrency is a documented non-transfer.
- v1 scope: rowid tables, primary B-tree only; secondary indexes etc. stay write-through.

## Status
- **Phase 0 DONE**: the 3 durability-agnostic leaf modules (`src/bf_mini_page.c`,
  `bf_circular_buffer.c`, `bf_mapping.c` + `bf_cache.h`) ported verbatim, whole-file guarded
  by `SQLITE_OMIT_BF_CACHE`, inlined into the amalgamation (`main.mk` `SRC +=`,
  `tool/mksqlite3c.tcl` list + `available_hdr`).
- **Phase 1 DONE**: read cache + write-through — `BfCache` lifecycle, config/pragma,
  `pcache2` activation, btree read hooks, descent shortcut.
- **Phase 2 DONE**: record-granular physiological WAL, now **ON by default**
  (`main.mk` adds `-DSQLITE_BF_INSERT_BUFFERING`).  Write-back insert/update/delete,
  commit-time record logging, recovery replay, checkpoint materialisation, forward AND
  reverse merge scans, merged `Count`, group commit (`PRAGMA bf_group_commit=N`) and
  mini-page compaction.  Differential gate ALL CLEAN.
  - Measured: **1 page frame per commit** (the WAL-format commit frame — zero base-page
    writes), and with `bf_group_commit=32` about **30x less WAL than stock** on
    single-row commits.  Counters live in `PRAGMA bf_cache_stats`.
- **Phase 3 IN PROGRESS**: the measurement campaign.  `bench/harness/` is the
  proper benchmark — a C driver linked against the amalgamation (prepared
  statements, per-op latency histograms, `/proc/self/io` block-layer bytes), a
  matrix runner that enforces an equal memory budget and a cgroup cap, and a
  report generator.  See `bench/harness/README.md`.  `--pre REV` builds a second
  amalgamation as a `bf_pre` SUT so a before/after runs inside ONE campaign;
  cross-campaign numbers are not comparable (stock alone has spanned 3.2x).

### Where the fork actually stands (2026-09-16, steady state, within-campaign)

| workload | vs stock |
|---|---|
| inserts | **3.96x** (was 0.22x before the 2026-09-15 fixes) |
| point reads, larger-than-memory, zipf 0.9 / 0.99 | **1.06x / 1.02x**, reading 1.20x / 1.29x fewer bytes |
| point reads, saturated ring (16 MiB / 4M rows) | 0.95x / 0.97x |
| point reads, warm in RAM, zipf 0.99 | 1.31x (transient -- cache still filling, no eviction) |
| update, mixed read/write | parity |

**Read these as steady state, and distrust any read number that is not.**  The
record cache keeps filling for minutes: the same workload and build measured
53.8% / 60.5% / 69.6% hit rate at 5 / 20 / 60 s of warmup, with `evictions=0`
throughout.  Earlier campaigns -- and an earlier version of this table, which
claimed 1.15x -- measured that transient.  `configs/steady.json` uses a 420 s
warmup and the numbers above only after `cached_records` plateaued across
repeats.  **Report `cached_records` and `evictions` beside every hit rate**; a
hit rate without them is uninterpretable.

**The one thing holding reads back is measured and consistent: FIFO retention.**
Every steady-state cell sits **18-25 points below the Zipf ideal** for the number
of records it actually caches (33.9% vs 54.7%; 53.7% vs 71.7%; 31.9% vs 57.2%;
49.2% vs 71.7%).  The ring evicts the oldest record, not the coldest, so it
keeps what was promoted recently rather than what is read often.  The REF bit
that would fix this already exists and is already set on every access --
`evictCallback` ignores it.  A REF-bit CLOCK policy is the next change with a
quantified prize behind it.

The second constraint is the benchmark environment, not the code: BF reads
20-29% fewer bytes and converts almost none of it into throughput, because under
buffered I/O an avoided miss is usually an OS page-cache hit.  `../bf-tree` uses
direct I/O precisely so a miss costs a device read.  Two cache-retention
experiments (copy-on-access second chance; bulk mini-page copy) were built,
measured and reverted for this reason -- see the memory notes.

The three findings this file used to list as unexplained:
  - *record-size cliff* — **explained and fixed**.  `aSizeClass` was filled
    descending and scanned ascending, so every mini-page became 4096 B.
  - *`negative_read` 4.5x slower* — **did not reproduce**; a stale-binary artefact.
  - *UPDATE never buffers* — **still true** (`rec frames/commit` = 0); it takes
    the page-image path at 1 frame/commit.  The oldest open item.

## Build & test
```bash
cd build && ../configure --quiet && make sqlite3          # BF build, Phase 2 ON
cc -O2 -DSQLITE_OMIT_BF_CACHE -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE \
   -I. -I../src -o sqlite3_stock shell.c sqlite3.c -lm -lz   # stock (no BF)
cd ../bench && sh stress.sh                                # differential oracle -> ALL CLEAN
sh stress_buf.sh                                           # same, vs ../build/sqlite3_buf
BF_GROUP=8 sh stress_buf.sh                                # ... with group commit on
BF_PROMOTION=100 sh stress_buf.sh                          # ... with read promotion at max
BF_CACHE_SIZE=262144 sh stress_buf.sh                      # ... with a ring small enough to CYCLE
sh wal_write_amp.sh 5000 200 1                             # write-amplification report

# Benchmark campaign (bench/harness/README.md documents the methodology)
sh bench/harness/build_suts.sh --all                       # every SUT, one amalgamation
python3 bench/harness/runner.py bench/harness/configs/smoke.json   # ~6 min sanity
python3 bench/harness/runner.py bench/harness/configs/full.json    # ~2-3 h campaign
python3 bench/harness/report.py bench/harness/results/full         # -> RESULTS.md

# A/B of our own src/ changes: bf_pre = a second amalgamation built from REV
sh bench/harness/build_suts.sh --pre HEAD                  # + bfbench_bf_pre
python3 bench/harness/runner.py bench/harness/configs/fixes_smoke.json  # ~2 min
python3 bench/harness/runner.py bench/harness/configs/fixes.json        # ~1.5 h
```
**Run the `BF_CACHE_SIZE` variant.**  At the default ring size these workloads
never evict -- every benchmark in this repo reports `evictions=0` and the oracle
databases are smaller still -- so eviction, free-list reuse and
upgrade-under-pressure went untested for the project's whole life.  That is how
a segfault in `bfFreeListRemove` (`bench/ring_repro.sh`) reached a commit.  With
`BF_CACHE_SIZE=262144` a single suite run produces ~2,200 evictions, ~10,300
upgrades and ~1,400 compactions.

Ablation switches (all default OFF, i.e. the feature is on): `SQLITE_BF_NO_MERGE_SCAN`,
`SQLITE_BF_NO_WRITEBACK_DELETE`, `SQLITE_BF_NO_DESCENT_SHORTCUT`,
`SQLITE_BF_NO_MINIPAGE_COMPACT`.

## Debug builds are the fastest diagnostic here

```bash
cd build && cc -O0 -g -DSQLITE_DEBUG -DSQLITE_BF_INSERT_BUFFERING \
   -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE -I. -I../src \
   -o sqlite3_dbg shell.c sqlite3.c -lm -lz
python3 ../bench/gen_stress.py 7 wal 2500 > /tmp/d.sql && ./sqlite3_dbg /tmp/d.db < /tmp/d.sql
```

`SQLITE_DEBUG` turns on hundreds of internal invariant checks in btree/pager/wal.
They pay for themselves: the 2026-09-15 wal-index corruption bug
(`bench/ckpt_repro.sh`) fired `walIndexAppend`'s own assert **immediately**,
while the release build only produced a confusing "database disk image is
malformed" several statements later, in a different operation.  Reach for this
before reaching for printf.

Two things to know:
- Do **not** add `-DSQLITE_OMIT_SHARED_CACHE` reflexively.  It silences the
  table-lock assert, which is a real check on BF's own cursors -- that assert is
  what pointed at `bfFlushOneMiniPage` opening a write cursor without the lock
  the VDBE would have taken.
- A debug build is far slower; use it for correctness, never for timing.

## Performance work: the method (measure, don't guess)

Every performance change follows this loop.  It exists because guessing already
cost us once: a "10x read-path regression" turned out to be the *write* path —
the benchmark's temp-table load — masquerading as reads.

1. **Build a workload that isolates ONE path.** Split phases and time them
   separately (`.timer on` prints per-statement time).  If a read benchmark
   contains inserts, it is a write benchmark.  Put helper data in a separate
   ATTACHed database so the measured run does no writes at all.
2. **Profile before touching code.**
   ```bash
   cc -O2 -g -fno-omit-frame-pointer -DSQLITE_BF_INSERT_BUFFERING ... -o sqlite3_bf_prof
   perf record -q --call-graph fp -F 999 -o p.data -- ./sqlite3_bf_prof db < w.sql
   perf script -i p.data | python3 bench/tools/flamegraph.py out.svg "title"
   perf report -i p.data --stdio -g graph,0.5,caller --percent-limit 2
   ```
   `bench/tools/flamegraph.py` is self-contained (Brendan Gregg's scripts are not
   installed here); it writes the SVG *and* prints a self-time table, which is
   what you act on.  `kernel.perf_event_paranoid=2`: user-space sampling of our
   own processes works, kernel tracing does not.  `valgrind --tool=callgrind` +
   `callgrind_annotate` give deterministic instruction counts when sampling is
   too noisy.
3. **Fix the frame the data names, then RE-PROFILE.** Do not assume the fix
   worked: our first `IsDirty` fix (early exit) looked obviously right and left
   the function at 82% of samples, because the hot case was CLEAN mini-pages
   that scan to the end.  The real fix was an O(1) flag.
4. **Re-run the differential oracles** after every perf change — several of these
   touch dirty-tracking, where a wrong answer silently loses writes.
5. **Stop only when what remains is structural**: documented design overhead
   (single-writer, SQLite's descent, WAL frame padding), not an accident.

**Be suspicious of every regression.**  The Bf-Tree paper reports wins on *every*
metric.  If we measure a slowdown, the null hypothesis is that OUR integration is
wrong — not that the paper does not transfer.  Cross-check the original Rust
implementation at **`../bf-tree/`**: `src/tree.rs`, `src/mini_page_op.rs`,
`src/nodes/`, `src/circular_buffer/`, `src/range_scan.rs`, `src/wal/`, and their
own harness in `benchmark/` (`bench_bftree.toml`, `bench_e2e.toml`, `run.sh`).
Only after showing our code matches theirs is "structural difference" an honest
conclusion.

### Measured baselines
Superseded by the table above.  The 2026-08-18 baselines (1.14x reads, 6.2x
appends, "1 page frame/commit") were taken before the size-class, truncate,
commit-logging and promotion fixes, and with `wal_autocheckpoint` forced to 0.
The commit-time claim still holds as a claim about **what a commit emits** (1.00
record frame); since auto-checkpoint is on by default it is no longer also a
claim about total bytes on disk — inserts reach 2.58 page frames/commit once
checkpoints materialise the records.  `write_amp` measures both.

## Testing specialists (`.claude/agents/`)
- **`esbmc-verifier`** — ESBMC bounded model checking of pure modules (WAL codec, mini-page,
  circular buffer, recovery idempotence).
- **`libfuzzer-tester`** — clang libFuzzer+ASan/UBSan on the WAL decoder/replay + end-to-end SQL.

Both are scoped to test **our** changes hard, not SQLite core (already trusted). Tools
installed: clang 22, esbmc 8.3.0. They reference `src/bf_wal.c` and `test/bf/` which appear
in Phase 2 — expected.

## Conventions
- Edit `src/`; `cd build && make sqlite3` regenerates `tsrc/` → `sqlite3.c`. Never edit
  `build/` or `tsrc/`.
- Every phase must pass the differential oracle (and from Phase 2, the crash-injection
  oracle) before commit.
