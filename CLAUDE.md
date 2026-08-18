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
- **Phase 3 NEXT**: the read-side benchmark harness — dataset >> RAM (cgroup cap or
  O_DIRECT VFS), Zipf-skewed point reads, BF sized well below the dataset, I/O counts
  reported next to latency.  That is where the paper's central claim gets tested.

## Build & test
```bash
cd build && ../configure --quiet && make sqlite3          # BF build, Phase 2 ON
cc -O2 -DSQLITE_OMIT_BF_CACHE -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE \
   -I. -I../src -o sqlite3_stock shell.c sqlite3.c -lm -lz   # stock (no BF)
cd ../bench && sh stress.sh                                # differential oracle -> ALL CLEAN
sh stress_buf.sh                                           # same, vs ../build/sqlite3_buf
BF_GROUP=8 sh stress_buf.sh                                # ... with group commit on
sh wal_write_amp.sh 5000 200 1                             # write-amplification report
```
Ablation switches (all default OFF, i.e. the feature is on): `SQLITE_BF_NO_MERGE_SCAN`,
`SQLITE_BF_NO_WRITEBACK_DELETE`, `SQLITE_BF_NO_DESCENT_SHORTCUT`,
`SQLITE_BF_NO_MINIPAGE_COMPACT`.

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
