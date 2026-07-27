# Plan: BF-Tree v2 — a SQLite fork with a record-granular physiological WAL

> Approved 2026-06-24. Portable copy of the plan (origin:
> `~/.claude/plans/wise-fluttering-dove.md`). This is the durable source of truth.
> The implementation happens in a **separate, fresh SQLite fork**; this repo
> (`ablation-phase0`) is the reference/source of ported modules.

## Context

The current fork (`ablation-phase0`) showed BF-Tree's mini-page record cache can layer on
SQLite's B-tree, but measured write-buffering as a *net loss*. Re-reading the VLDB'24 paper
(§5.7) and the reference impl (`microsoft/bf-tree`) corrected our model of *why*:

- **The paper's wins come from the mini-page buffer pool, not from durability.** Durability
  is *"orthogonal to the core design… disabled for all baselines."* Headline numbers (6× write,
  2× point, 2.5× scan) run with **direct I/O, fsync and WAL disabled**.
- **Durability, when on, is ONE ARIES-style *physiological* redo WAL**: *"Before any write is
  committed it appends a log entry to the WAL and waits (log full or default 1 ms interval)
  until flushed. The log entry points to a page on disk and describes an operation (insert,
  delete)."* **Group-commit, not per-commit fsync.** Checkpointing = async WAL replay (Aurora-
  style); recovery = rebuild pages + replay WAL ops.

**Why the current fork loses, and what this fork fixes.** The current fork pays a **commit-
time full-page flush** (logical-replay dirty mini-pages → base pages → 4 KB WAL frames) under
**strict per-commit fsync**, on an **OS-page-cache-dominated box**. To get the paper's **write**
win we must persist a *small record* at commit instead of a *4 KB page* (the file-format change)
**and** relax to group-commit. The paper's **read/scan** wins need *no* format change — just the
buffer pool we already have, measured under **direct I/O on a larger-than-RAM dataset** (never
exercised before). This fork delivers both.

### Locked decisions
- **Durability mechanism:** **one** record-granular **physiological WAL** — extend SQLite's WAL
  with a record-frame type. BF rowid-table leaf mutations log as `[pgno, op, key, val]`;
  everything else (schema, secondary indexes, overflow, freelist, splits) stays page-image
  frames. One log, one recovery walk. (No sidecar; no cross-log atomicity.)
- **Durability strength:** **configurable** — group-commit (~1 ms, paper-faithful) *and* strict
  per-commit fsync, selected via `PRAGMA synchronous`. **Measure both**; the trade-off is a
  thesis result.
- **Concurrency:** single-writer (keep `BtShared` serialization); documented non-transfer.
- **Fork base:** fresh upstream SQLite clone; **port the 3 durability-agnostic leaf modules
  verbatim** (`bf_mini_page.c`, `bf_circular_buffer.c`, `bf_mapping.c`); reimplement btree-hooks
  + WAL from scratch.

## Target architecture

**Write path.** Insert/delete on a BF rowid table → mini-page (RAM, existing buffer pool) →
append a **physiological record** to the WAL. Base leaf pages are **not** written at commit;
they are written lazily at **eviction** (mini-page too large/cold) and at **checkpoint**. Commit
appends a commit marker and flushes the WAL per the durability mode (group-commit vs fsync).

**WAL frame-format extension.** Keep frames fixed page-size (preserves `walFrameOffset`
arithmetic and checksums). Add a frame **kind** flag in the frame header: a **record-batch
frame** packs a sequence `[pgno u32][op u8][keyLen varint][valLen varint][key][val]…` up to
page-size, instead of a single page image. Commit frames still carry the `nTruncate` marker.
Keys use the existing canonical `bfEncodeRowid` (8-byte big-endian).

**Read path / wal-index.** SQLite's wal-index hashes `pgno → latest frame` (page images). Extend
it so a reader reconstructing page P = latest page-image of P (DB file or WAL) **+ in-order
replay of record-ops targeting P** from later record-batch frames, up to the reader's `mxFrame`.
Maintain an in-memory `pgno → [(frame, offset)…]` index built during wal-index construction.
The mini-page cache absorbs most of this cost in steady state; single-writer removes race
complexity. (Constraint: physiological-WAL DBs require a BF-aware reader; a stock binary cannot
read the WAL — acceptable, and the differential oracle runs separate DBs anyway.)

**Checkpoint (= the paper's async replay).** Page-image frames apply by pgno as today; a
record-batch frame applies by loading its target page, re-applying its ops, writing the page
back. After checkpoint the WAL resets. This is *"replay the WAL to the page."*

**Recovery.** SQLite's normal recovery rebuilds B-tree structure from DB-file pages (this is the
paper's snapshot step "(1)"). Then WAL recovery replays frames to the last commit marker:
page-image frames as today, record-batch frames by re-applying ops to their pages (step "(2)").

**Rollback / savepoints.** Uncommitted WAL tail (frames after the last commit) is discarded as
today; the BF cache drops the matching dirty mini-page records. Savepoints record the WAL frame
position; `ROLLBACK TO` truncates the WAL tail + dirty records to that position. Simpler than v1
because there is no "flush-at-savepoint-open" invariant — records were never applied to base.

**v1 scope (keeps it tractable).** Rowid tables, primary (table) B-tree only. Secondary indexes,
overflow, schema → page-image frames (write-through), so a committing txn still emits **one**
ordered WAL stream with **one** commit marker (atomicity trivial). Buffering secondary-index
records is a later phase if Phase 4 shows it pays.

## Deliverable 1 — Knowledge-transfer doc (`KNOWLEDGE.md` in the new repo)

Written first. Distilled from this session's exploration + memory. Must contain:

- **Paper-accurate durability model** (§5.7 above) so the implementer doesn't re-derive it.
- **Data-structure field tables:** `BfMiniPage` (24 B header), `BfKVMeta` (8 B: offset,
  `keyLenAndOp` 14+2, `valueLenAndRef` 15+1, 2 B preview), record types (Table 1: insert/cache/
  tombstone/phantom × dirty?/exists?), `BfAllocMeta` state lifecycle, circular buffer, mapping.
  Reference current `src/bf_cache.h`.
- **The 5 mini-page bugs to avoid by construction** (from `write-buffering-bugfixes`): insert
  offset frontier = MAX+len; compute maxOffset before the meta shift; `bfBinarySearch` branch
  direction; canonicalize the rowid delete key; tombstone in-place update bypasses the
  free-space gate.
- **Edge-case map:** per-leaf keying (`ownerPgno`) + `rootPgno` group-flush; eviction tri-state
  (`evictCallback`); copy-on-access + reference-bit cold-record eviction (paper §5.2); descent
  shortcut; merge-iterating scans + tombstone suppression + re-insert-over-tombstone shadow
  collapse; negative-search/phantom caching (§5.6); autovacuum unsupported; index BF caching
  off (relevant to the secondary-index phase).
- **Build & oracle setup** and the remaining divergences (concurrency; v1 scope; relaxed-vs-
  strict durability).

## Deliverable 2 — Staged implementation plan (each stage gated on validation)

**Phase 0 — Bootstrap & harness.** Fresh upstream clone (pinned tag). Port the 3 leaf modules +
headers verbatim. Wire `PRAGMA bf_*`, config, build. Port `bench/stress.sh` + generators; build
`sqlite3` vs `sqlite3_stock` (`-DSQLITE_OMIT_BF_CACHE`). *Acceptance:* hooks-off build is
differential-clean.

**Phase 1 — In-memory buffer pool (write-through, no new WAL).** Reimplement read path (payload
hook → mini-page lookup + promotion; phantom caching; per-leaf keying; descent shortcut) and
write-through buffering. *Acceptance:* `stress.sh` ≥18 seeds × {delete,wal,memory} + integrity
+ SQLITE_DEBUG asserts clean. (Re-establishes a known-good baseline on the fresh base.)

**Phase 2 — Physiological WAL (the core).** New `src/bf_wal.c` (record-frame encode/decode,
pgno→ops index). Extend `wal.c` frame kind + `pager.c` write path so BF leaf mutations emit
record-batch frames and commit does **no** base-page write. Extend wal-index + read-time page
reconstruction. Recovery replays record-frames. *Acceptance:* differential clean **and** the
crash-injection oracle clean across every fsync/commit boundary; `bf_cache_stats` shows zero
base-page writes at commit for the buffered workload.

**Phase 3 — Group-commit, checkpoint, eviction-flush.** Group-commit flusher (~1 ms / log-full)
mapped to `PRAGMA synchronous`; checkpoint applies record-frames by replay; flush-capable
eviction (dirty slab drains via WAL-backed writeback instead of refusing) + copy-on-access with
reference-bit cold eviction. *Acceptance:* small-buffer torture (`bf_cache_size`=256 KB) sustains
buffering (no dirty-head stall); crash-injection across checkpoint boundaries clean; strict and
group-commit modes both differential/recovery-clean.

**Phase 4 — Measurement campaign.** Direct-I/O, larger-than-RAM, Zipfian (skew 0.9) YCSB-like
matrix: write-amp (bytes/op), point, scan; group-commit vs strict; BF vs stock. Commit an
ablation table + `RESULTS.md`. *(Optional Phase 5: secondary-index record buffering if the win
justifies it.)*

## Testing strategy (mirrors the paper's, plus formal methods)

The paper itself validates with **differential fuzzing vs a reference model**, **libFuzzer +
AddressSanitizer**, and a static mini-page reference model. We mirror and extend:

- **Differential oracle (top gate):** ported `stress.sh`/`stress_buf.sh` + generators; byte-
  identical `.out` + `.dump` vs `-DSQLITE_OMIT_BF_CACHE` stock. Every phase.
- **Crash-injection oracle (Phase 2–3 core):** a test VFS that records writes/fsyncs and cuts
  power at each fsync boundary; reopen → recover → `integrity_check` + match an allowed committed
  state. Drives WAL replay + checkpoint correctness. Reuse SQLite's VFS-shim pattern from `test/`.
- **Fuzzing:** libFuzzer + ASAN on (a) the **record-frame decoder + recovery** fed malformed
  WALs (must reject/recover, never UB), and (b) SQL with BF on (reuse `test/dbfuzz2.c`,
  `test/ossfuzz.c` patterns).
- **ESBMC (bounded model checking on the pure, pointer-bounded modules):** harnesses with
  `__ESBMC_assume`/`assert` for: record-frame **encode∘decode = identity** + bounds-safety on
  truncated input; mini-page **insert/offset-frontier non-overlap** + binary-search correctness;
  circular-buffer head/tail/state-machine invariants; **checkpoint/recovery idempotence** (replay
  twice = replay once). Keep harnesses small; the amalgamation is out of ESBMC's reach.
- **Property-based testing:** model-based — a reference model (the generators already track a live
  row dict; or a simple in-memory map) asserted against the engine over random op-sequences;
  plus C-level PBT (generator + shrinker, e.g. `theft`, or extend `reduce.py`) on the mini-page
  and WAL-codec modules.

## Critical files

- **Port verbatim (from current `src/`):** `bf_mini_page.c`, `bf_circular_buffer.c`,
  `bf_mapping.c` + their `bf_cache.h` declarations.
- **New:** `src/bf_wal.c`/`.h` (physiological record frames + pgno→ops index); `KNOWLEDGE.md`;
  `test/bf/` crash-injection VFS, libFuzzer + ESBMC harnesses.
- **Reimplement / modify:** btree hooks in `src/btree.c` (payload/insert/delete/scan/moveto,
  commit, savepoint/rollback); **WAL frame kind in `src/wal.c`**; **write/read/checkpoint/recovery
  paths in `src/pager.c`**; config/pragmas in `src/bf_config.c`.
- **Reference (read-only, current fork):** `src/bf_btree.c` (`bfApplyOneRecord`, eviction
  tri-state), `src/bf_pager.c` (the stubbed journal to supersede), `BF_TREE_DESIGN.md` §7/§13.

## Verification (how to run)

- Build: `cd build && make sqlite3`; stock: `cc -DSQLITE_OMIT_BF_CACHE … -o sqlite3_stock`.
- Correctness: `sh bench/stress.sh` (≥18 seeds × 3 journal modes) → "ALL CLEAN".
- Durability: crash-injection oracle — every recovery point passes `integrity_check` and matches
  an allowed committed state, in **both** strict and group-commit modes.
- Formal: `esbmc test/bf/{wal_codec,minipage,cbuffer,recovery}_harness.c` → no assertion / array-
  bounds violations within bound.
- Fuzz: `clang -fsanitize=fuzzer,address test/bf/fuzz_bfwal.c …` → hours, zero crashes/leaks.
- Win: Phase 4 direct-I/O larger-than-RAM scripts emit write-amp + point/scan tables into
  `RESULTS.md`, group-commit vs strict, BF vs stock.

## Reference sources

- Paper: Hao & Chandramouli, *Bf-Tree: A Modern Read-Write-Optimized Concurrent Larger-Than-
  Memory Range Index*, PVLDB vol.17 no.11, 2024. PDF: https://vldb.org/pvldb/vol17/p3442-hao.pdf
  (durability = §5.7; eviction/copy-on-access = §5.2; range scan §5.3; negative search §5.6).
- Reference impl (Rust): https://github.com/microsoft/bf-tree (see `doc/snapshot-recovery.md`).
- Companion docs: https://github.com/XiangpengHao/bf-tree-docs

## Future work — the "faithful" branch (evaluate both approaches)

The staged plan above keeps SQLite's fixed-page `btree.c` and inserts Bf-Tree as a record
cache *beside* it. This is a pragmatic engineering choice — reuse SQLite's B-tree, keep the
differential oracle applicable, land correctness incrementally — **not** a file-format
compatibility requirement. v2 does **not** care about on-disk compatibility with stock SQLite
(a lesson from v1): it is only a convenient invariant for the differential oracle, not a goal.

The reference (`microsoft/bf-tree`) is **variable-size and record-granular natively**: a logical
leaf resolves through the page table to `PageLocation::{Mini, Full, Base, Null}`, where `Mini`,
`Full`, and `Base` are the *same* `LeafNode` at different `node_size` (up to
`MAX_LEAF_PAGE_SIZE = 32 KB`), a mini-page chains to its base page, and reads consult the
mini-page delta before falling through. The fixed-page adaptation therefore leaves performance on
the table: the larger-than-memory win comes from **memory density** (RAM holds only hot records
as compact mini-pages), and a fixed-page pcache that also pins full 4 KB pages for the same
leaves dilutes exactly that advantage.

**Plan:** once the file-compatible v2 (Phases 0–4) is complete and measured, branch and build a
**file-incompatible, maximally faithful** variant, then evaluate the two head-to-head. The
faithful branch should transfer the Bf-Tree design decisions the fixed-page adaptation cannot,
squeezing peak performance:
- **Variable-size leaves as the on-disk unit** — replace SQLite's fixed-page leaf format (own
  file format; break stock compatibility deliberately). Leaves are `LeafNode`s of varying
  `node_size`; mini/full/base are one type at different sizes.
- **Mini-page-native B-tree navigation** — the leaf level addresses `PageID -> PageLocation`
  through the mapping table (Mini/Full/Base/Null), with the mini-page chained over its base
  page, rather than reconstructing a 4 KB page for `btree.c`.
- **Density-first buffer pool** — the circular buffer (mini-pages) is the dominant resident
  representation of hot data; full/base pages fall back to disk under direct I/O. Promotion
  Mini→Full and copy-on-access follow the paper (§5.2).
- **Direct-I/O, larger-than-RAM path** as a first-class mode (the regime the paper's wins live
  in), not an afterthought.

**Evaluation goal:** quantify how much of Bf-Tree's headline advantage the pragmatic fixed-page
integration captures vs. what the faithful, file-incompatible design recovers — reporting resident
bytes (mini vs full), point/scan/write throughput, and write amplification for both, so the thesis
can state the cost of SQLite-shaped integration explicitly. Correctness for the faithful branch
can no longer lean on the byte-identical `.dump` oracle; use the model-based / property oracle
(reference row-map vs engine) instead.
