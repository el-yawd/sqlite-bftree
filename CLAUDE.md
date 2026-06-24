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
- **Phase 0 DONE** (commit after the 3.52.0 baseline): the 3 durability-agnostic leaf
  modules (`src/bf_mini_page.c`, `bf_circular_buffer.c`, `bf_mapping.c` + `bf_cache.h`)
  ported verbatim, whole-file guarded by `SQLITE_OMIT_BF_CACHE`, inlined into the
  amalgamation (`main.mk` `SRC +=`, `tool/mksqlite3c.tcl` list + `available_hdr`), inert
  (no hooks), differential gate **ALL CLEAN (54 tests)**.
- **Phase 1 NEXT**: reimplement the read path + write-through buffering — port the `BfCache`
  lifecycle (`bf_cache.c`) + config/pragma (`bf_config.c`) + `pcache2` activation in
  `main.c` + btree read hooks; re-establish a stress-clean baseline.

## Build & test
```bash
cd build && ../configure --quiet && make sqlite3          # BF build (default)
cc -O2 -DSQLITE_OMIT_BF_CACHE -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE \
   -I. -I../src -o sqlite3_stock shell.c sqlite3.c -lm -lz   # stock (no BF)
cd ../bench && sh stress.sh                                # differential oracle -> ALL CLEAN
```

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
