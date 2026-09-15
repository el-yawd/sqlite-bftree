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
  report generator.  See `bench/harness/README.md` for the requirements it meets
  and the workloads it runs.  **Not yet run at full scale** — the campaign
  (`configs/full.json`, ~2-3 h) is the next thing to execute.
  - Already surfaced by the smoke pass, and *not* yet explained:
    - **Write-back insert refuses records above ~64-96 B** and falls back to the
      base-page path (100% buffered at 64 B, 31% at 96 B, 14% at 200 B —
      independent of `bf_cache_size` across a 16x range, so structural, not
      capacity).  `wal_write_amp.sh` uses 64-byte payloads, i.e. exactly the
      size where buffering always succeeds, so the "1 page frame/commit" claim
      was only ever measured inside the good region.  `record_size` is now an
      axis in `configs/full.json`.
    - **UPDATE never buffers at all**: `wal_record_frames=0`,
      `buffered_inserts=0` on a pure-update workload; it takes the page-image
      path (still 1 frame/commit, so ~2x less WAL than stock, but none of the
      record-granular win).
    - **`negative_read` is ~4.5x SLOWER than stock** while reading 5x fewer
      bytes from disk — a CPU-bound path, the opposite of the paper's §5.6
      phantom-caching claim.

## Build & test
```bash
cd build && ../configure --quiet && make sqlite3          # BF build, Phase 2 ON
cc -O2 -DSQLITE_OMIT_BF_CACHE -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE \
   -I. -I../src -o sqlite3_stock shell.c sqlite3.c -lm -lz   # stock (no BF)
cd ../bench && sh stress.sh                                # differential oracle -> ALL CLEAN
sh stress_buf.sh                                           # same, vs ../build/sqlite3_buf
BF_GROUP=8 sh stress_buf.sh                                # ... with group commit on
BF_PROMOTION=100 sh stress_buf.sh                          # ... with read promotion at max
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
Ablation switches (all default OFF, i.e. the feature is on): `SQLITE_BF_NO_MERGE_SCAN`,
`SQLITE_BF_NO_WRITEBACK_DELETE`, `SQLITE_BF_NO_DESCENT_SHORTCUT`,
`SQLITE_BF_NO_MINIPAGE_COMPACT`.

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

### Measured baselines (2026-08-18, 8M-row / 0.83 GiB db, warm)
- read path (point reads, no writes): **1.14x wall / 1.30x CPU vs stock**
- appends (200k rows, temp table): was **132x** stock -> **6.2x** after the
  `BF_MINI_F_DIRTY` fix (17.5s -> 0.83s); remaining cost is the per-mutation
  whole-map walk in `bfFlushTableDirty` (~29% of samples), which wants per-root
  dirty tracking next
- commits: **1 page frame/commit** (WAL-format floor, zero base-page writes);
  `bf_group_commit=32` gives ~30x less WAL than stock

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
