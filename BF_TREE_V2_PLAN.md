# BF-Tree v2 — the plan

**The single planning document for this fork.**  It merges what used to be four
overlapping files: `BF_TREE_V2_PLAN.md` (the staged route), `BF_TREE_V2_PARITY_PLAN.md`
(reference gaps, Stages A–D), `BF_TREE_V2_PERF_PLAN.md` (the write-path profile work) and
`BF_TREE_V2_AGENT_HANDOFF.md` (live state, ordered backlog, progress log).  Those four
disagreed with each other about what was done; one file cannot.

`BF_TREE_V2_KNOWLEDGE.md` stays separate on purpose — it is the *map* (paper model, field
tables, bugs to avoid by construction), not a plan, and it is read first.

---

## 1. How to work in this repo

1. Read, in order: `BF_TREE_V2_KNOWLEDGE.md`, then this file, then the source and benchmark
   files for the task.
2. **Source and measured artifacts are truth.  Prose here is a claim to verify.**  Every
   status line in §3 carries the `file:line` or commit that backs it; if you find it wrong,
   fix the line and log the correction — do not work around it.
3. Before editing, read `git status` and the relevant diff.  Preserve uncommitted work; do
   not revert it merely because it is uncommitted.
4. One bounded item at a time.  Profile or reproduce before changing performance-sensitive
   code (§6 is the method).
5. **A mechanism that cannot be switched off does not land.**  Every mechanism ships with a
   `SQLITE_BF_NO_*` switch or a PRAGMA *and* a counter, or it is unattributable at the
   **H3 ablation campaign** and invisible to the oracles when it silently does nothing.
6. Validation per change is **correctness only** (§6.1).  Full campaigns run at the named
   H3/H4/H5/H6 campaign gates, not per change.
7. **Before ending any turn that changed code, tests, benchmark config, docs, or a design
   decision, update this file:** §2 current state, the relevant backlog item, and a dated
   entry appended to §9.  Record commands actually run and their exact outcomes.
8. Never quote smoke/tripwire throughput as a result.  Mark it `NOT QUOTABLE`.
9. Never report a hit rate without `cached_records` and `evictions`; add bytes/record and
   records/mini-page where available.
10. If stuck, log the blocker in §9 with the smallest reproducer, the diagnostics, the
    hypotheses already ruled out, and the next thing to try.  Do not hide it.
11. Do not rewrite old progress-log entries.  Append corrections; mark superseded
    conclusions explicitly.
12. Edit `src/`.  Never edit `build/`, `tsrc/`, or the amalgamation.

## 2. Current state — verified 2026-09-21

> **2026-09-24 — read this box first; it supersedes the D1 state below it.**  A new
> crash-differential oracle (`bench/crash_oracle.py`) and a full-ROLLBACK generator
> (`bench/gen_rollback_stress.py`) found **eight correctness bugs, five of them committed-data
> loss or torn transactions**, all in code committed before today and all invisible to every
> existing oracle.  All eight are fixed in the **uncommitted** working tree (WAL payload **v3**,
> `BFWAL_OP_CLEAR`).
> See the 2026-09-24 entry in §9 for evidence, the D1 item for the per-bug list, and §3.2 for
> what the write path now does.  **Every throughput and WAL-volume number in this file predates
> these fixes and is unverified against them** — rollbacks now replay the log, flushes log a
> CLEAR per leaf, and group commit no longer defers a commit that writes pages.  Nothing has
> been re-measured (correctness-only gate, §6.1).

### 2.1 Repository

* Branch `main` at **`b0026ba`** *"ring: file a freed block under a class it actually fits,
  and audit the rest"*.

The working tree holds three unrelated categories of change.  Read them separately — only the
first is what V0 gates.

**1. Source changes, under V0.**  `src/bf_btree.c`, `bf_cache.{c,h}`, `bf_circular_buffer.c`,
`bf_config.c`, `bf_mapping.c`, `bf_mini_page.c`, `bf_wal.{c,h}`, `pager.{c,h}`.  This is the
**dead-code sweep**: 37 unreachable symbols removed, including the `bfGlobalCache` singleton and
the `SQLITE_CONFIG_BFCACHE`/`_SIZE` opcodes — which were not merely dead but a latent collision,
30 being SQLite's own `SQLITE_CONFIG_ROWID_IN_VIEW`.  The diff was **+22 / −653 at 2026-09-21**,
and that ratio is the point: it is a deletion sweep, and **no behavioural change is intended**.
**It has not been built or gated.  That is V0.**

**2. Plan consolidation.**  This file rewritten in place; `BF_TREE_V2_PARITY_PLAN.md` and
`BF_TREE_V2_PERF_PLAN.md` removed; the untracked `BF_TREE_V2_AGENT_HANDOFF.md` removed after its
content was absorbed; `CLAUDE.md` repointed.

**3. Untracked `tfg/`** — the thesis write-up (`tfg/paper.typ`).

`bench/harness/configs/steady.json` shows as modified, but only a stale document reference inside
a comment array was repointed.  **No benchmark configuration changed** — in particular its
`warmup_seconds` was already 900 and was not touched.

> Correction, 2026-09-21: the previous handoff recorded HEAD as `54f4ae6` and described the
> free-list class-selection fix as the pending behavioural change in the working tree.  That
> fix is **committed** (`b0026ba`), and `BF_TREE_V2_PARITY_PLAN.md` was committed with it.

### 2.2 Phase status

| phase | state |
|---|---|
| **0** — port the 3 durability-agnostic leaf modules | **DONE** — `bf_mini_page.c`, `bf_circular_buffer.c`, `bf_mapping.c`, `bf_cache.h`, whole-file guarded by `SQLITE_OMIT_BF_CACHE`, inlined into the amalgamation |
| **1** — read cache + write-through | **DONE** (`8dba75c`) — lifecycle, config/pragmas, `pcache2` activation, btree read hooks, descent shortcut |
| **2** — record-granular physiological WAL | **DONE** (`3bf3fb2`), **ON by default** (`main.mk` adds `-DSQLITE_BF_INSERT_BUFFERING`) — write-back insert and **delete**, commit-time record logging, recovery replay, checkpoint materialisation, forward and reverse merge scans, merged `Count`, transaction grouping, mini-page compaction. WAL payload v2 persists both leaf and table-root pgno, fixing the 2026-09-22 recovery corruption; **v3 (2026-09-24, uncommitted)** adds `BFWAL_OP_CLEAR` so replay never re-applies a flushed op, and ROLLBACK rebuilds the cache from the log instead of emptying it. **Existing-row UPDATE is buffered since 2026-09-29** (D3-core + D3a, `SQLITE_BF_NO_UPDATE_BUFFER` to ablate; §3.2). Two contracts remain open: **D1** (remaining recovery/checkpoint edge cases) and **D2** (what an acknowledged commit guarantees) |
| **3** — measurement campaign + reference parity | **IN PROGRESS** — this is where all remaining work in §5 lives |
| **4** — the faithful file-incompatible branch | not started; §7.3 |

### 2.3 The numbers that are currently defensible

Steady state, **within campaign**, single-threaded, buffered I/O, 100 B values, scrambled
Zipf, cgroup-constrained.  Cross-campaign numbers are not comparable — stock alone has spanned
3.2x.

| workload | vs stock | accompanying evidence |
|---|---|---|
| inserts | **3.96x** (was 0.22x before the 2026-09-15 fixes) | |
| point reads, larger-than-memory, zipf 0.9 | 1.06x | 1.20x fewer read bytes; 33.9% record hit |
| point reads, larger-than-memory, zipf 0.99 | 1.02x | 1.29x fewer read bytes; 53.7% record hit |
| point reads, saturated ring (16 MiB / 4M rows), zipf 0.9 / 0.99 | 0.95x / 0.97x | 31.9% / 49.2% record hit |
| update, mixed read/write | parity | |
| WAL volume, `bf_group_commit=32`, inserts | ~**30x less** than stock — 264 vs 8,636 B/op | 1.00 record frame per **WAL commit**, where **983 WAL commits absorbed 30,720 SQL transactions** (`group_deferred = 30048`) |

Provenance for that last row: `bench/harness/results/sweeps/results.jsonl`, experiment
`group_commit`, dataset v64, `insert=100`.  **Read its denominator carefully.**  `wal_commits`
counts WAL commit *events*, not SQL transactions: a deferred transaction stages nothing
(`bf_btree.c:583-590`), dirties no page, and `sqlite3PagerCommitPhaseOne` returns early
(`pager.c:6684-6688`) without calling `sqlite3WalFrames` at all.  So "1.00 record frame per
commit" at group 32 means roughly **one frame per 31 committed transactions**, not one frame per
transaction — and the metric silently embeds the deferral window that D2 is about.  The same
number at `group_commit=1` would mean something entirely different, and **we do not currently
quote it** (H6).  `report.py`'s column is labelled `rec frames/commit`, which is where the
ambiguity comes from.

Write-path caveat: with auto-checkpoint on, inserts reach **2.58 page frames/commit** once
checkpoints materialise the records.  "One frame per commit" is a claim about what a *commit
emits*, not about total bytes on disk.  `write_amp` measures both.

**Read these as steady state, and distrust any read number that is not.**  The record cache
keeps filling for minutes: the same workload and build measured 53.8% / 60.5% / 69.6% hit rate
at 5 / 20 / 60 s of warmup with `evictions=0` throughout.  900 s is the larger-than-memory warmup floor **for `steady`'s shape, and only for it**
— 420 s never evicted there, so every number taken at 420 s was mid-fill.  The floor does not
transfer to another shape: `steady.json` records that the fill is driven by **operations, not
wall clock** (a build 16% faster cached 17% more records in the same 420 s).  See H2.  Two earlier
claims (1.15x reads, and the "corrected-budget" 1.077x/1.229x) were that transient.

The historical steady-state cells above predate the derived size classes and copy-on-access, and predate the current cache split.  **They
are not measurements of the current tree.**

### 2.4 Two measured constraints currently dominate the evidence

1. **FIFO retention — CORRECTED 2026-09-24 (H1b): mostly a metric artifact.**  The figures
   below are lookup ratios that read 10–15 points low; per read, the engine matches a model of
   its own policy and copy-on-access recovers ~2 points and no throughput.  Kept for the record:
   Every steady-state cell sits **18–25 points below the Zipf ideal** for
   the number of records it actually caches (33.9 vs 54.7; 53.7 vs 71.7; 31.9 vs 57.2; 49.2 vs
   71.7).  The ring evicts the oldest record, not the coldest.  Addressed by the
   copy-on-access second-chance region (landed `c99c040`).  The mechanism is not in doubt — the
   comparison is against the Zipf ideal **at the occupancy each cell actually reached**, so it
   already controls for how much is cached, and the ring's order is FIFO by construction.  What
   is unmeasured is **how much of the gap the remedy recovers**: the region *without* the REF
   discard returned +1.2 points of hit rate for −3.6% ops, and the landed pairing has no
   steady-state number at all (H1b).  The fix is not a CLOCK policy — CLOCK is in neither the
   paper nor `../bf-tree`.
2. **Buffered I/O masks the value of avoided reads, while code-parity gaps remain under
   investigation.**  BF reads 20–29% fewer bytes and converts almost none of it into throughput,
   because under buffered I/O an avoided miss is usually an OS page-cache hit.  This is not yet
   grounds for saying the shortfall is the environment rather than our code: full-page admission
   is absent (M2), phantoms still read the leaf, the fixed-page pcache is still duplicated
   alongside the ring, and the workload differs from the paper's in four ways (§2.6).  `../bf-tree` uses direct I/O precisely so a miss costs a device read.  Two
   cache-retention experiments were built, measured and reverted for this reason — the
   copy-on-access region *without* the REF-bit discard measured **+1.2 points of hit rate for
   −3.6% ops**, and a bulk mini-page copy fared no better.  The discard is what is supposed to
   make the copy pay for itself, by shrinking the page as it relocates; that pairing is what
   `c99c040` landed and what is still unmeasured.

### 2.5 Findings closed, so nobody re-opens them

* **Record-size cliff** — explained and fixed: `aSizeClass` was filled descending and scanned
  ascending, so every mini-page became 4096 B.  3x more records cached; hit rate 22.7% → 50.4%.
* **`negative_read` 4.5x slower** — did not reproduce; a stale-binary artefact.
* **"The ring reclaims no space from freed blocks"** — retired by the same size-class fix.  The
  free list is correct and is consulted first, exactly like `circular_buffer/mod.rs:539`.
* **"Concurrent reads race on a global cache"** — false; it read dead code.  The cache is
  **per pager** (`pager.c`, `sqlite3PagerOpenBfCache`).  The real problem is the opposite: N
  connections allocate N independent rings.  See S1.
* **Hit rate is admission, not eviction** (at default ring sizes) — the ring almost never
  evicts; hit rate is limited by promotion admission and grows with warmup.
* **`evictCallback` ignored `pEvictProtect`** — fixed 2026-09-21.  The upgrade and compaction
  paths in `sqlite3BfRecordWrite` set that field before their evict-and-retry so the sweep
  cannot take the slab they are copying *from*; the sweep never looked at it, and only the
  copy-on-access path (`bf_cache.c:739`) ever did.  The failure is silent data loss, not a
  wasted copy: the sweep unlinks a CLEAN mini-page with `locType = BF_LOC_NULL`, the caller
  re-points `pEntry->pPage` **without restoring `locType`**, and every later
  `sqlite3BfRecordRead` on that pgno returns `BF_NOT_FOUND` at its `locType` check — the
  record is present and invisible.  Observed directly: `sqlite3BfRecordWrite` returning
  `BF_OK` and `sqlite3BfRecordRead` returning `BF_NOT_FOUND` for the same leaf and key one
  instruction apart, and one lost `INSERT` in a cross-table dump diff against stock.
  Fixed at the root (the sweep refuses a protected slab) plus both re-point sites now writing
  `locType` and `pPage` together.

  **Reachability is UNPROVEN, and an earlier claim here that it was reachable without M1 was
  wrong.**  A binary with the fix reverted passes the original repro, the whole of
  `stress_xtable.sh`, and a read-then-write workload built specifically to produce the state,
  as long as M1's same-leaf guard is in place.  The only demonstrated trigger is flushing a
  leaf and immediately buffering into it, which is M1's stall drain and nothing else in the
  tree.  So the fix makes a declared invariant true and is cheap, but it is **defensive** —
  treat it as closing a latent defect, not a live corruption.
* **Mid-session `PRAGMA wal_checkpoint` corruption** — fixed 2026-09-15; BF record frames
  occupied a wal-index slot without `walIndexAppend`, so nothing zeroed the hash block after a
  log restart.  `gen_stress.py` now issues checkpoints.

### 2.6 Why the read win is small here: the configuration, quantified

This is the arithmetic behind "why don't we stand out on zipf".  It is not one missing
feature; it is a configuration in which the mechanism has little room to pay.  It is the
rationale for **H0**.  H0's harness work has mostly landed; **H2's exact-shape warmup proof**
remains the gate before mechanism results are publishable.

**Our workload is not the paper's** (`benchmark/src/bench_bftree.rs` and `common.rs`, against
`bench/harness/configs/steady.json`):

| | reference (`bench_bftree.toml`) | ours (`steady.json`) |
|---|---|---|
| record | key 16 B, **value = the key** → ~32 B | key + **100 B value** → ~116 B |
| zipf | `rand_distr::Zipf` rank used **directly as the key** — hot keys contiguous | **scrambled** through FNV (`bfbench.c:610`) — hot keys scattered |
| cache : data | 1 GiB : 3.2 GB ≈ **30%** | 256 MiB : 6.8 GB ≈ **3.8%** |
| threads | **30** | **1** |

Each row independently shrinks the effect.  Record size alone is 3.6x: a 32 B record against
a 4 KiB leaf is a ~128x memory-amplification argument; a 116 B record is ~35x.  Scrambling is
why our mini-pages hold **1.26 records each** (`live_mini_pages` 414,574 vs `cached_records`
522,780) — we pay a header, meta and size-class round-up per *single* record.

This cuts both ways and the honest report keeps both arms: YCSB scrambles, so our config is
the more conservative one, and a mechanism that needs 32 B records and clustered hot keys to
win is a *finding*, not a failure.  What is not defensible is reporting only the adversarial
config while citing the paper's numbers as the target.

**The per-record cost of the ring is the capacity limit.**
`mini_page_bytes / cached_records` = **250 B to cache a 116 B record**.  A 128 MiB ring at
250 B/record holds the ~522 k records we measured, against stock's ~65 k cached leaf pages —
and **8x the hot-set coverage buys ~12 points of hit rate**, because zipf 0.9 over 60 M items
has a fat tail.

### 2.7 Derived size classes: a capacity result, not yet a throughput result

`sqlite3BfInitSizeClasses` implements the reference's derivation (`tree.rs:222-250`):
`class(k) = 2^k * (base + sizeof(BfKVMeta)) + sizeof(BfMiniPage)`, cache-line aligned.  It is
the single definition feeding both the `BfFreeList` and `BfCache` copies of the ladder, which
were duplicated — a disagreement between them would file a block under one class and hand it
back as another.

The base is `PRAGMA bf_min_record`, default 64.  It **has to be configuration**, and the
reason is measured.  At a fixed base of 64, 4M rows under a 16 MiB ring:

| value_len | recs/page | B/record | cached |
|---|---|---|---|
| 32 | 5.4 | −2.5% | +1.4% |
| 64 | 2.3 | −12% | +11.7% |
| 100 | 1.5 | **−23.6%** | **+27.7%** |
| 200 | 1.2 | **+23.6%** | **−18.6%** |

A single fixed base is a 28% capacity win on one workload and a 19% capacity loss on another.
Shipping that as "the faithful ladder" would tune the engine for our own benchmark and
penalise the replication — backwards for a change justified by fidelity.  The v200 row is why
this is known: it was included as a falsification test and it fired, within 1.4 points of the
predicted size.

**But "set it to the record size" is NOT the rule.**

| dataset | old ladder | base 64 | base = record size |
|---|---|---|---|
| v100 (116 B record) | 240 | **182** | 211 |
| v200 (216 B record) | 263 | 325 | **262** |

v100 is *better* at base 64 than at its own record size of 108, while v200 is far better at
208.  The relation is not monotonic, and the v100 case shows up as a change in mini-page
**occupancy** (1.75 vs 1.57 records/page) rather than in class selection, which the class
arithmetic does not explain.  Unknown mechanism; measure per workload rather than computing
the base.

Caveat: throughput moved 2–3% at most, and on the larger-than-memory shape the warmup sweep
showed 20% more cached records buying *nothing* in hit rate.  **Judge this on `B/record`, and
expect it to matter only where capacity binds** — at 16 MiB / 4M rows it does (hit rate +0.8
pts at v100); at 256 MiB / 60M rows it does not.

Provenance correction (2026-09-21): an earlier version of this argued that the reference
"sets `cb_min_record_size` per workload".  **It does not** — `config.rs:27` sets
`DEFAULT_MIN_RECORD_SIZE = 4` and nothing in `benchmark/` or `dev/` overrides it, including
the configs that produced the paper's numbers.  Our 64 is OUR tuned value.  The measurements
stand; only the appeal to the reference's authority was false, and it was doing real work in
the argument.  **Whether base 4 beats base 64 for us is an open question** (H1a), not an
assumption in either direction.

## 3. What is implemented, and where it stops

Each line is verified against source at `b0026ba` + the dirty tree, 2026-09-21.

### 3.1 Read / cache path

`INSERT`, `DELETE`, `CACHE` and `PHANTOM` mini-page records · per-leaf mapping with canonical
rowid keys · point lookup through mini-pages · a clean-hit descent shortcut that can avoid
loading the leaf · configurable read promotion · forward and reverse merge scans · tombstone
suppression and reinsert-over-tombstone · merged `Count` · separate page-cache and
record-cache counters · dynamic pcache hash resizing · mini-page compaction under write
pressure · derived size classes (`PRAGMA bf_min_record`) · copy-on-access second-chance region
(`PRAGMA bf_copy_on_access`) · REF-based cold-record shedding during that copy.

Stops at:

* a cached `PHANTOM` does not short-circuit descent early enough to avoid the first leaf
  load — negative caching preserves semantics, not the I/O win;
* copy-on-access relocates **clean** mini-pages only, and only after a served point hit;
* upgrade-time cold shedding is compile-time opt-in (`SQLITE_BF_UPGRADE_SHED`,
  `bf_cache.c:929`), default off;
* SQLite's fixed page cache stays resident beside the ring, diluting the density advantage
  that is the whole larger-than-memory argument.

### 3.2 Write / WAL path

Physiological record batches inside SQLite's WAL stream · write-back rowid inserts and
deletes · dirty/unlogged lists so commit does not walk the whole map · WAL payload v2 records
both target leaf and owning table root · a recovery-time pgno→ops index retaining that root ·
transaction grouping · checkpoint-time materialisation into page-image frames · record/page
frame and cache counters.

**Replay ordering and rollback, as of 2026-09-24 (payload v3, uncommitted):**

* **`BFWAL_OP_CLEAR`.**  Every successful mini-page flush in WAL mode records its leaf
  (`bfNoteFlushed`, `bf_btree.c`); the commit carrying the flush's page images logs a CLEAR per
  leaf *ahead of* its own dirty records (`bfLogClears`), including the checkpoint's forced-flush
  commit (`sqlite3BfBtreeLogFlushMarks`).  Replay starts each leaf after its last CLEAR
  (`bfReplayOnePage`, `bf_cache.c`).  A first design inferred supersession from page images
  instead and was broken by the crash oracle (§9): a flush writes a leaf's records wherever
  their keys live *now*, which after a split is another leaf.
* **Every indexed op carries its WAL frame number.**  Ops past `mxFrame` are the torn tail of a
  crashed commit and are pruned after recovery (`walIndexRecover`); at a rollback they are
  committed group batches a spill wrote and are kept, then pruned before any frame number can
  be reused (`sqlite3WalBfPrune`).  `pBfWal` is cleared at log restart and pruned at savepoint
  undo.
* **ROLLBACK rebuilds, it does not empty.**  `sqlite3BfBtreeRollbackRehydrate`: clear the cache,
  then replay (1) the WAL's ops, (2) payloads staged in the rolled-back transaction, (3) the
  open group batch — for a full ROLLBACK and for `ROLLBACK TO` the savepoint that began the
  transaction (`iSavepoint<0`).  Records from (1) are marked logged again unless the rollback
  discarded frames carrying some; (2) and (3) are re-logged by the next commit.  A rollback of
  a read transaction touches nothing.  Cost: O(ops in the WAL) per write-transaction rollback.
  **Unmeasured.**
* **Group commit defers only record-only commits.**  A commit that dirtied pages, or carries a
  CLEAR, stages the whole open batch with it (`sqlite3PagerBfHasDirty`).
* **wal-index**: `walFrames` clears a dead writer's stale hash entries *before* writing when
  any slot it is about to fill holds one, because record frames leave holes that defeat
  `walIndexAppend`'s lazy cleanup (`bench/torn_tail_repro.sh`).
* **Reverse range seeks merge** (`bfMergeSeekRev`, `btree.c`).  They used to fall back to a
  flush that a read transaction refuses, so `WHERE id<=K ORDER BY id DESC` skipped buffered rows.

These are correctness mechanisms, and they deliberately have **no off switch** — disabling any
of them re-enables a measured data-loss bug.  Each has a counter in `PRAGMA bf_cache_stats`
(`replay_applied`, `replay_superseded`, `replay_torn`, `replay_dropped`, `rollback_rehydrate`,
`clear_logged`); `replay_dropped` must stay 0.

Stops at:

* **existing-row UPDATE is buffered (D3, 2026-09-29).**  An overwrite of a row with a base
  cell becomes a `BFOP_INSERT` upsert on its leaf that SHADOWS the base cell; the point seek's
  exact-match exit parks on the record (`btreeBfParkOnMini`, counter `shadow_serves`), the merge
  scans already let it win, and the flush's `sqlite3BtreeInsert` overwrites the base cell.
  Declined, as before: `BTREE_SAVEPOSITION` (index-driven multi-row UPDATE), preformat,
  zero-extended and overflow-sized values (`update_fallbacks`).  Update commits now emit 1 record
  frame and 0 page frames (tripwire).
* **dirty mini-pages cannot be evicted.**  `evictCallback` (`bf_cache.c:1148-1169`) returns
  `BF_ERROR` on a dirty mini-page rather than merging it to base.
* checkpoint materialises cached records into page-image frames before normal backfill; it
  does not replay record frames directly to their target pages.
* **`bf_group_commit=N` is bounded deferred durability, not group commit.**  The first N−1
  *record-only* commits increment `nDeferred` and return **without staging anything**, so a
  crash loses acknowledged transactions in the open group.  This contradicts §4.2's locked
  decision ("group commit, ~1 ms, paper-faithful"), which specifies a shared flush every
  committer *waits* for.  Resolving that contradiction is item **D2**.  (Until 2026-09-24 it also
  deferred the records of commits that wrote pages, tearing those transactions on a crash;
  fixed, see D1.)

### 3.3 Reference parity matrix

| mechanism | status | the difference |
|---|---|---|
| record op types, sorted mini-pages | high | ours are deltas beside fixed pages, not the native leaf representation |
| circular ring + allocation state machine | high, under single-writer | plain transitions replace concurrent CAS |
| physiological record logging | conceptually high | inside SQLite's WAL rather than the reference's compact WAL |
| REF marking | partial | we mark exact hits; the reference marks the lower-bound candidate before the equality test (`leaf_node.rs:1658`) |
| copy-on-access region | partial | clean mini-pages, successful point hits only |
| cold-record discard | partial | copy-on-access matches; size-upgrade discard is default-off |
| size-class derivation | partial | seven classes, 4096 B ceiling, base 64 vs the reference's default 4 |
| free list | partial | class direction now matches (`b0026ba`); stale-chain handling is weaker |
| point lookup | good, within rowid scope | the full-page pcache remains alongside |
| negative caching | partial | phantom does not avoid the initial leaf read |
| forward/reverse scan | semantically good | the reference uses promotion and full-page retention differently |
| **dirty eviction / merge** | **missing** | the dirty FIFO head refuses eviction |
| **full-page / gap cache (`BF_LOC_FULL`)** | **absent** | zero producers — the identifier appears only in a comment at `bf_mapping.c:18` |
| **separate scan promotion** | **absent** | no `scan_promotion` symbol exists |
| **eviction batching** | **absent** | `sqlite3BfCacheEvict(pCache, 16)` at three call sites — a fixed **16 entries**, against the reference's ~1024 accumulated **bytes** with retry cap 10 (`tree.rs:1012-1018`) |
| **general UPDATE buffering** | **done 2026-09-29** (single-row; SAVEPOSITION declined) | see §3.2, D3 |
| native mini/base split | deliberately absent | SQLite materialises and balances fixed pages |
| shared reader buffer pool | missing | one `BfCache` per pager (`pager.c:4259-4269`) |
| concurrent readers | missing | `BfCache.mutex` is allocated (`bf_cache.c:217`) and the `bfCacheEnter`/`bfCacheLeave` macros exist (`bf_cache.h:637-641`), but **have zero call sites**.  Scaffold only — do not read it as locking. |
| direct I/O / io_uring | deliberately absent | the primary environment mismatch for reads |
| CPR snapshot | deliberately absent | replaced by the WAL/checkpoint design |
| write concurrency | deliberately absent | `BtShared` single-writer stays |

### 3.4 Ablation switches present in source

`SQLITE_BF_NO_MERGE_SCAN`, `SQLITE_BF_NO_WRITEBACK_DELETE`, `SQLITE_BF_NO_DESCENT_SHORTCUT`,
`SQLITE_BF_NO_MINIPAGE_COMPACT`, `SQLITE_BF_NO_COPY_ON_ACCESS`, `SQLITE_BF_NO_DIRTYLIST`,
`SQLITE_BF_NO_UNLOGLIST`, `SQLITE_BF_NO_DROPCLEAN`, `SQLITE_BF_NO_MAXROWID`,
`SQLITE_BF_NO_UPDATE_BUFFER` (D3a; D3-core's shadow serve is correctness and has no switch),
`SQLITE_BF_NO_BLIND_INSERT` (D3b),
`SQLITE_BF_UPGRADE_SHED` (opt-**in**).  All `NO_` switches default off, i.e. the feature is on.

PRAGMAs (authoritative list: `tool/mkpragmatab.tcl:410-436`): `bf_cache` (enable),
`bf_cache_size`, `bf_cache_stats`, `bf_promotion_rate`, `bf_group_commit`, `bf_min_record`,
`bf_copy_on_access`.

**Ordering matters, in three rules, and every one of them fails silently.**  A knob that reads
back correctly but has no effect is almost always this.  Issue them in exactly this order:

1. **`PRAGMA bf_cache` — before anything creates a pager.**  It is the one *global* setting:
   enabling it calls `sqlite3_config(SQLITE_CONFIG_PCACHE2, &bfMethods)` (`bf_cache.c:673`),
   which by SQLite's own contract affects only pcaches created **afterwards**.  Get this one
   late and the run has no record cache at all while still reporting plausible numbers.
2. **Then `PRAGMA journal_mode`, then the per-cache settings.**  `bf_cache_size`,
   `bf_min_record`, `bf_promotion_rate`, `bf_group_commit` and `bf_copy_on_access` all reach
   into *this connection's* `BfCache` (`bf_btree.c:724`, `:753`, `:776`), and `journal_mode`
   reopens the pager and drops anything configured before it.  (Correction 2026-09-24: `bf_copy_on_access`
   is the exception — a global the ring re-reads at every (re)initialisation, so it is
   order-free; H1b.)
3. **`bf_min_record` after `bf_cache_size`.**  Setting it rebuilds the size-class ladder by
   dropping every mapping and reinitialising the ring at its *current* capacity, so it must see
   the capacity the run actually wants (`bfbench.c:875-877`).  The reverse order silently builds
   a ladder for the wrong ring.

`bench/harness/bfbench.c:852-880` implements all three; `bench/harness/README.md:92-96`
documents the first two.

## 4. The design, and what is locked

### 4.1 The insight that drives everything

Bf-Tree's wins come from the **mini-page buffer pool, not from durability**.  The paper's
headline numbers (6x write, 2x point, 2.5x scan) run with **direct I/O, fsync and WAL
disabled** — durability is *"orthogonal to the core design… disabled for all baselines."*

Two consequences the whole project rests on:

* the **write** win needs the file-format change (commit persists a small record, not a
  4 KiB page) *and* group commit;
* the **read/scan** wins need no format change at all — just the buffer pool, measured on a
  larger-than-RAM dataset.  On an OS-page-cache-dominated box there is little to win,
  because an avoided miss is usually a page-cache hit rather than a device read.

Durability in the paper (§5.7) is **one ARIES-style physiological redo WAL**: before a write
commits it appends a log entry — *"points to a page on disk and describes an operation"* —
and waits for the flush (log full, or a 1 ms interval).  Group commit, not per-commit fsync.
Checkpointing is async WAL replay; recovery is rebuild-then-replay.

### 4.2 Locked decisions

* **One** record-granular **physiological WAL**, extending SQLite's own WAL with a record
  frame kind. BF rowid-table leaf mutations log as `[leafPgno, rootPgno, op, key, val]`;
  everything else
  (schema, secondary indexes, overflow, freelist, splits) stays page-image frames.  One log,
  one recovery walk.  No sidecar, no cross-log atomicity.
* **Configurable durability**: group commit *and* strict per-commit fsync via
  `PRAGMA synchronous`.  Measure both — the trade-off is a thesis result.
  **Amended by owner decision, 2026-09-24 (D2):** the durable, paper-faithful mode is
  `bf_deferred_commit` ≤ 1 — every commit's records reach the OS before it returns, as in the
  reference — with `PRAGMA synchronous=FULL` adding an fsync.  `bf_deferred_commit=N>1` (old
  name `bf_group_commit`, kept as an alias) is **bounded deferred durability**, named as such:
  up to N−1 acknowledged commits may be lost even to a process crash.  A reference-style
  waiting group commit is not built: it only pays with concurrent committers, which a
  single-writer engine does not have.
* **Single writer.**  `BtShared` serialization stays; a documented non-transfer.  Read
  concurrency is in scope (item **S1**).
* **v1 scope**: rowid tables, primary B-tree only.  Secondary indexes stay write-through.
* **No on-disk compatibility requirement.**  Byte-identical `.dump` against stock is a
  convenient oracle, not a goal.  A physiological-WAL database already needs a BF-aware
  reader.

### 4.3 Target architecture

**Write path.** Insert/delete on a BF rowid table → mini-page in the ring → append a
physiological record to the WAL.  Base leaf pages are **not** written at commit; they are
written lazily at eviction and at checkpoint.  Commit appends a commit marker and flushes per
the durability mode.

**WAL frame format.** Frames stay page-size (preserving `walFrameOffset` arithmetic and
checksums).  A frame *kind* flag marks a **record-batch frame** packing
`[leafPgno u32][rootPgno u32][op u8][keyLen varint][valLen varint][key][val]…`
up to page size.  Commit frames
still carry `nTruncate`.  Keys use the canonical 8-byte big-endian `bfEncodeRowid`.

**Read path / wal-index.** A reader reconstructs page P as: latest page image of P (file or
WAL) **+** in-order replay of record ops targeting P from later record frames, up to the
reader's `mxFrame`.  An in-memory `pgno → [(frame, offset)…]` index is built during wal-index
construction.  The mini-page cache absorbs most of this in steady state.

**Checkpoint (= the paper's async replay) — TARGET, not current behaviour.** Page-image frames
apply by pgno; a record-batch frame applies by loading its target page, re-applying its ops,
writing it back.  **The code does something else:** `bfCheckpointMaterialize()` turns cached
records into page-image frames ahead of normal backfill (§3.2).  Which of the two is the
intended architecture is an open decision, deferred to D1's evidence — the trade is paper
fidelity and fewer written bytes against keeping SQLite's backfill as the single base-page
writer, which is a real asset for a fork whose top gate is a byte-identical `.dump`.  It is also
measurable: materialisation is what puts inserts at 2.58 page frames/commit under
auto-checkpoint.

**Recovery.** SQLite's normal recovery rebuilds structure from the database file (the paper's
step 1); WAL recovery then replays to the last commit marker, record frames included (step 2).

**Rollback / savepoints.** The uncommitted WAL tail is discarded as today, and the BF cache
drops the matching dirty records.  Savepoints record the WAL frame position.  Simpler than the
v1 fork, because records were never applied to base pages.

### 4.4 Verified reference citations — audited 2026-09-21, do not redo

Every `*.rs:NNN` citation in the planning documents, in `CLAUDE.md` and in `src/` was checked
against `../bf-tree`, on the rule that *a performance change must be justified by the
reference, so the comparison stays clean*.  Of the twenty claims in the original audit, **17 were
correct and 3 wrong** — and all three wrong ones were in prose doing argumentative work,
which is the dangerous kind.  The table below adds the write-path citations from the
perf plan, checked the same way.

| claim | verdict |
|---|---|
| `tree.rs:222-250` size-class derivation `2^k*(base+meta)+header`, cache-line aligned | **correct** (it also de-duplicates equal classes; our port should too) |
| `tree.rs:1012` `TARGET_EVICT_SIZE = 1024` + retry cap | **correct** — cap is 10, and it counts BYTES |
| `leaf_node.rs:1528` `discard_cold_cache && op.is_cache() && !is_referenced()` | **correct** |
| `leaf_node.rs:1658` REF set on read, before the absent test | **correct** |
| `mini_page_op.rs:744` upgrade copies with `discard_cold_cache = true` | **correct** |
| `mini_page_op.rs:151` / `storage.rs:332` copy-on-access to tail | **correct** |
| `circular_buffer/mod.rs:429` threshold `capacity * (1 - ratio)` | **correct** |
| `circular_buffer/mod.rs:539` the ring tries the free list first | **correct** (ours does too) |
| `config.rs:38` `scan_promotion_rate` distinct from read | **correct** |
| `config.rs:47` `cb_copy_on_access_ratio` | **correct** |
| `config.rs:155` `write_load_full_page` default true | **correct** |
| `wal/mod.rs:196` `Wal::append_and_wait` copies one `write_op` into a shared buffer and waits for its LSN; a background thread flushes | **correct** — cost per mutation is O(1); nothing walks a dirty set |
| `mini_page_op.rs:474` an insert never reads the base page: a leaf at `PageLocation::Base(offset)` gets a newly allocated mini-page with `next_level = offset` and the record written blind | **correct** — the base page is read only for the root leaf, for CPR snapshots, and when merging back |
| `leaf_node.rs:1713` `new_size_if_upgrade` returns `None`, the caller merges and then promotes to a full page | **correct** |
| `OpType::is_cache()` = `Cache` or `Phantom` | **correct** |
| *"64 is the reference's `cb_min_record_size` default"* | **WRONG** — it is **4** (`config.rs:27`), never overridden in `benchmark/` or `dev/` |
| *"its benchmark sets `min_record` per workload"* | **WRONG** — nothing sets it.  Still live in `src/bf_config.c:78-79`; filed under H1a |
| *"a mini-page past 2 KB merges with its leaf"* | **WRONG** — 2048 is marked `// Deprecated`; the bound is derived and the trigger is different (M2) |

Two structural differences that were not claims at all, and matter more than the citations:

* **Free-list class order.**  The reference keeps the tree's ladder ASCENDING and the free
  list's copy DESCENDING on purpose (`freelist.rs:106`, `size_classes.reverse()`), with an
  explicit index mapping (`:125`).  Unifying ours to one ascending array was fine, but
  `bfFreeListAdd` then filed a block under the smallest class **≥** its size, where the
  reference files under the largest class **≤** its size (`:112`, `:152`).  Ours was the
  **unsafe** direction: a block filed under a class larger than itself can be handed to a
  request that overflows it — harmless only because every `sqlite3BfCircularBufferAlloc` call
  site passes an exact class size.  **Fixed** in `b0026ba`.
* **Full-page cache is a MODE on the read path**, not an addition.  See M2.

**There is no per-commit cache sweep in the reference at all** — Bf-Tree has no pager and no
`xTruncate` hook, so the `bfCacheTruncate` cost (once 50% of write cycles) was 100%
integration overhead we added; SQLite's own `pcache1TruncateUnsafe` showed the cheap way, and
that is what `e6d399e` did.  Likewise nothing in the reference walks a dirty set at commit.

**Method note for the next audit: check the CALL GRAPH, not just that the cited line says
what it is said to say.**  `Consolidate` was cited correctly and was still unreachable, and
`BfCache.mutex` is allocated by code that never enters it.

## 5. The backlog — one ordered list

Prefixes are stable: **V** validation gate, **D** durability/correctness, **M** mechanism
(reference parity), **H** harness/measurement, **S** structural project.  Each item names the
merged documents it came from, so nothing is silently dropped.

Three ordering rules override personal preference:

* **Correctness before capacity before speed.**  A mechanism measured on an ungated tree is
  measured on nothing.
* **Rank by measured headroom, not by how large the gap looks in the parity matrix.**  M1 was
  ranked the top gap on the strength of a code reading — `evictCallback` refuses dirty pages,
  therefore the ring stalls.  It does not: the fallback path already flushes the table, and
  M1 rescued 2 inserts in 58,244.  An item earns its rank from a counter, and the cheapest
  way to get that counter is often to build the mechanism behind a switch and look.
* **No feature is evaluated before the comparison is fair.**  The premise of the old Stage A
  stands: the budget arithmetic (A1, **done** — `runner.py:222-270`, `split_memory()` rounds the ring up
  and raises the total, verified) and the workload shape (H0) decide whether any M item has
  room to pay.

**Current ranking, after the M1 measurement (2026-09-21).**  The items with measured headroom
are the write path and the unswept retention knobs, not the eviction machinery:

0. **D1's post-recovery tree corruption — FIXED 2026-09-22.** WAL payload v1 omitted the
   table root, so recovery treated a leaf as a root; the first replay split nested a new tree
   below the real one. Payload v2 persists and restores `rootPgno`; `recover_repro.sh` is clean
   at 250 and 2000 rows in all three checkpoint orders.
0b. **Eight more D1 correctness bugs — FIXED 2026-09-24, UNCOMMITTED** (several were
   committed-data loss on ordinary SQL; see D1).  Committing them is the next action once the gate in §9 is
   green; nothing downstream is quotable on a tree that loses rows on `ROLLBACK`.
1. **D1 / D2** — the remaining durability contracts. Nothing downstream is quotable until an
   acknowledged commit means something definite.
2. ~~**H1b**~~ — **DONE 2026-09-24**: retention is not the constraint (see the H1b item).
3. ~~**D3-core → D3a**~~ — **DONE 2026-09-29** (see the items); measure at the D letter gate.
4. **H0 / H2** — update and scan arms at the paper shape, and a warmup floor per arm.
5. **M2** — full-page cache, the last absent mechanism with real upside, but it needs H0's
   contiguous-zipf arm to be judged fairly.
6. **D3b, D4, M3, M4** — narrower.
7. **S1** — still a project, still last.

**M1 is done and is NOT in this ranking** — see its entry below the active items.

---

### V0 — gate the current working tree — **DONE 2026-09-21, committed `f5f09dd`**
*From: handoff P0/B0.*  All binaries were rebuilt first; the ones on disk predated the sweep,
and stale binaries have produced false findings here before.  The sweep compiled with zero
warnings in all three configurations, which is itself evidence about a −653-line deletion.

| gate | outcome |
|---|---|
| `stress.sh` | ALL CLEAN 54/54 |
| `stress_buf.sh` | ALL CLEAN 216/216 |
| `BF_CACHE_SIZE=262144 stress_buf.sh` | ALL CLEAN 216/216 |
| `bench/ring_repro.sh` | OK, ring cycled without crashing |
| `BF_GROUP=8 stress_buf.sh` | 27/216, 0 failures — **STOPPED EARLY** at the owner's request |
| `BF_PROMOTION=100 stress_buf.sh` | **NOT RUN**, same reason |

The cycling-ring run is the one that carries weight: measured on the same shape it produced
515,664 evictions and 827,552 upgrades, so the eviction, free-list and upgrade paths this
diff touches actually executed.  A green suite at the default ring size would have proven
nothing about them.

- [ ] owed: full `BF_GROUP=8` and `BF_PROMOTION=100` runs
- [ ] owed: `bench/ring_repro.sh` under a `SQLITE_DEBUG` build — it ran release-only

> Process note, recorded because the rule was in this file at the time: the V0 item said
> *"do not commit unless the user explicitly asks"*.  The sweep and the plan consolidation
> were committed (`f5f09dd`, `bd338af`) on the strength of a "do it now" that answered a
> message proposing exactly that, which is an authorisation but not an explicit one.  Both
> are plain reverts if the owner disagrees.

### D1 — recovery and checkpoint ownership — **EIGHT MORE BUGS FOUND AND FIXED 2026-09-24 (UNCOMMITTED); CHECKPOINT DECISION OPEN**
*From: handoff P1a.*  Source: `bf_cache.c` (`bfReplayOnePage`, `sqlite3BfCacheReplayWal`),
`btree.c` (`bfCheckpointMaterialize`), `bf_btree.c` (`bfTagLeafRoot`), `wal.c`.
Repro: **`sh bench/recover_repro.sh`** (its header carries the full evidence).

> **BF crash recovery leaves a structurally corrupt B-tree.**  After SIGKILL with committed
> record frames still in the WAL, `PRAGMA integrity_check` reports
> *"Tree 2 page 2 cell 24: Child page depth differs"*.
>
> Three controls make it ours, and narrow it:
> * every committed row is present and readable — the count is exact — so this is a
>   **tree-shape defect, not data loss**;
> * **stock SQLite under the identical crash pattern is clean** in all three scenarios;
> * the same workload with a **clean close is clean**, in-session and on reopen, so the crash
>   is necessary;
> * correction after tracing: explicit checkpoint materialisation performs the bad split;
>   the earlier “before checkpoint” chronology mistook implicit materialisation for pure recovery.
>
> Root cause: BF WAL v1 stored only the target leaf. Recovery-created mini-pages therefore
> defaulted `rootPgno` to that leaf. For the 250-row reproducer checkpoint opened its replay
> cursor with `pgnoRoot=5` although the table root was page 2. Row 246 split page 5 into a new
> root over pages 6/7 while page 5 remained a child of page 2, exactly producing differing
> child depths. The earlier conclusion that the leaf→root gap was unrelated was false because
> it checked row survival, not structure.
>
> Fix: BF WAL payload v2 logs `[leafPgno, rootPgno, op, key, val]`; the recovery index retains
> the root and `bfReplayOnePage` restores it onto the mini-page. The 250- and 2000-row crash
> reproducers pass all three checkpoint orders.

**2026-09-24: eight more, found by `bench/crash_oracle.py` and `bench/gen_rollback_stress.py`.**
Every one was in committed code and invisible to every existing oracle.  Each has a
reproducer that fails on the session-start binary and passes on the fixed one.

| # | bug | severity | reproducer | fix |
|---|---|---|---|---|
| 1 | **any full ROLLBACK destroyed committed buffered rows** — `sqlite3BtreeRollback` emptied the record cache; `BEGIN;ROLLBACK` with no writes, a failed `COMMIT`, or an autocommit statement failing a constraint was enough.  Rows vanished at once and for good at the next clean close | **committed-data loss** | 3 rows committed, `BEGIN;ROLLBACK`, reopen → 1 row (the oracles never ran a full ROLLBACK, only `ROLLBACK TO`) | `sqlite3BfBtreeRollbackRehydrate` rebuilds the cache from the log; read-txn rollback touches nothing |
| 2 | **recovery replayed ops a flush had already put in base** — an UPDATE/DELETE that reached base through the page path came undone at the next checkpoint; `count(*)` double-counted in between | **committed-data loss** after a crash | buffered rows, a bulk insert that flushes them, page-path UPDATE + DELETE, SIGKILL → update reverted, deleted row back, count 2924 for 2903 | `BFWAL_OP_CLEAR` (payload v3), §3.2 |
| 3 | **recovery replayed the record frames of a torn commit** | atomicity after a crash | `bench/torn_tail_repro.sh` on the old binary: 420 rows for 400 | ops carry their frame; prune past `mxFrame` |
| 4 | **group commit deferred the records of a commit that also wrote pages** — a crash kept the page half ("wrong # of entries in index") | atomicity, group mode | `BF_GROUP=8 crash_oracle.py`: 22/40 failed | defer only record-only commits |
| 5 | **a torn tail whose first frame is a record frame** made the next write delete its own wal-index entry; a checkpoint then backfilled a stale page 1 ("invalid page number 151") | file corruption after a crash | `bench/torn_tail_repro.sh` (deterministic; fails without the fix) | stale-slot probe before `walFrames` writes |
| 6 | **reverse range seeks skipped buffered rows** under a read transaction — `WHERE id<=K ORDER BY id DESC` | wrong query results | 2 autocommit inserts, `id<=239 DESC` → only 6 | `bfMergeSeekRev` |
| 7 | **`ROLLBACK TO` the savepoint that began the transaction destroyed committed buffered rows** — it is `sqlite3BtreeSavepoint(ROLLBACK, -1)`, no pager savepoint was ever opened for it, so the flush-before-savepoint invariant never ran, and the cache was emptied | **committed-data loss** (WAL) | `INSERT … 412; SAVEPOINT s1; INSERT INTO u …; ROLLBACK TO s1;` → 412 gone | treat `iSavepoint<0` as a full rollback: rehydrate |
| 8 | **an UPDATE with `BTREE_SAVEPOSITION` of a row still in the mini-page** (e.g. driven through an index) took the same-size overwrite on a cursor serving the row from a scratch buffer → `SQLITE_CORRUPT` | statement fails ("malformed") | 2 inserts into an indexed table, `UPDATE u SET b='' WHERE a=30` | materialise + re-seek whenever `bfOnMini` survives the write-back block |

Found along the way and **not** fixed: `PRAGMA integrity_check` *inside* a write transaction with
buffered rows reports `wrong # of entries in index` (its physical cell count does not see
buffered rows; queries do, and it is clean once the transaction ends) — a false positive, both
journal modes, pre-existing.  And the buffered max-rowid is monotonic (`bf_cache.h`,
"never cleared"), so after a rollback or a delete of the top rows BF assigns larger rowids than
stock — documented as allowed, observable, not data loss; and `bfReplayOnePage`'s old claim that
a refused (`BF_FULL`) op "is re-applied at the next checkpoint from the base path" is false —
nothing reads record frames at checkpoint — so a refusal is now counted (`replay_dropped`).

My own first attempt at #2 (supersede an op when a newer *page image* of its leaf exists, plus
two flushes to keep that exact) was wrong twice over: a one-leaf flush rebalanced dirty siblings
and a scan returned rows out of order, and even with whole-table flushes the crash oracle found a
lost update, because a flush can land a leaf's records on another leaf.  CLEAR replaced the
design.  A third mistake of mine: resetting the flushed-leaf list on *every* cache clear, which
includes a savepoint rollback whose pre-savepoint flushes survive; the next full rollback then
resurrected a pre-flush value.  The list is now reset only by a whole-transaction rollback.  All
three were caught by the new oracles before landing — and the same oracles showed that the
overwrite flush of the first design had been *masking* bug #8.

- [x] **root-cause and fix the post-recovery tree corruption above** — fixed 2026-09-22
- [x] checkpoint immediately after recovery, without first descending the table —
      `recover_repro.sh` scenarios B and C; no rows lost
- [x] persist and restore `rootPgno` across recovery — WAL payload v2; the leaf→root gap was
      the structural defect even though the old row-count-only control showed no loss
- [ ] reproduce recovery into a mini-page that reaches `BF_FULL` — the "re-applied from the base
      path" claim is **false** (2026-09-24); such a refusal loses the record.  Now counted
      (`replay_dropped`); a reproducer that makes it non-zero is still owed
- [x] multiple table roots — `crash_oracle.py` runs four tables, one created mid-stream, one
      with a secondary index (2026-09-24)
- [x] crash during checkpoint; compare against the allowed committed states —
      `crash_oracle.py` mode `killckpt` (2026-09-24)
- [x] full ROLLBACK, torn commits, group commit under crash — bugs 1, 3, 4, 5 above
- [ ] crash oracle under `SQLITE_DEBUG` at scale (6 seeds run, §9) and with `BF_PROMOTION=100`
- [ ] **decide the checkpoint architecture** (§4.3): direct replay of durable record frames to
      their target pages, or materialisation via `bfCheckpointMaterialize()` as today.  If this
      work finds a state rehydration cannot reconstruct, the question answers itself; otherwise
      it is a design call on the trade named in §4.3, and it is the user's

### D2 — settle the durability contract  *(a live contradiction, not just a gap)*
*From: handoff P1b, and §4.2's locked decision, which current source does not implement.*

**Evidence gathered 2026-09-24 (read this before choosing).**

*What the reference actually does* (`../bf-tree`, audited with citations):
* With its WAL enabled, every insert/delete **blocks until its record is `pwrite()`n** as part
  of a shared batch (`wal/mod.rs:196-229`, `append_and_wait` waits on `flushed_lsn`); a
  background thread flushes every `flush_interval` (default **1 ms**, `config.rs:585`) or when the
  buffer fills.  The committer holds its page write guard while it waits (`tree.rs:997-1000`).
* **No fsync, anywhere on the WAL path** (`mod.rs:72-89`); "durable" means handed to the OS.
* The WAL is **off by default** (`config.rs:55`) and **off for every benchmark**
  (`benchmark/src/wrappers/bf_tree_wrapper.rs:15-43`; the RocksDB baseline sets `disable_wal`).
* Its recovery (`snapshot.rs:1423-1458`) is documented "Incomplete": it replays deletes as inserts,
  ignores LSNs, and its reader expects a tag byte the writer never writes.  No test exercises it.
* There are no transactions: each op is its own record.

*What that means here.*  The reference's batching only pays when several threads commit at
once; a single-threaded committer would wait up to 1 ms for nobody.  Its guarantee — the record
reaches the OS before the commit returns — is exactly our **`bf_group_commit=1`**, which we
already provide, with `PRAGMA synchronous=FULL` adding a real fsync on top (verified: the
record-only commit is refused whenever sector padding could be needed, so an fsync is issued
exactly when stock would issue one).  **`bf_group_commit=N>1` does something the reference never
does: it defers the `write()` itself**, so up to N−1 acknowledged commits are lost even on a
process crash (the crash oracle asserts precisely that bound, `BF_GROUP=8`, 160/160).

*What the deferral buys* (tripwire, NOT QUOTABLE — `wal_write_amp.sh 5000 200 1`, single-row
commits, fixed build): WAL bytes/commit stock 7,581; BF group 1: 4,120 (1.84×); group 8: 515
(14.7×); group 32: 124 (61×).  `synchronous` changes none of these.  **The whole group-mode WAL
saving is the deferral**; a "true" group commit that writes every commit falls back to the group-1
bytes.  The previously quoted "~30× less WAL than stock" is a deferred-durability number.

- [x] choose one and write it down — **(b), owner decision 2026-09-24**, on the evidence above:
      (a) as §4.2 phrased it is not what the reference does, and in a single-writer engine it
      degenerates to `N=1`.  Renamed: `PRAGMA bf_deferred_commit` (alias `bf_group_commit`);
      window documented at the pragma (`bf_config.c`) and the batch (`bf_btree.c`)
- [x] (a)-vs-(b) preference — superseded by the decision above
- [x] crash-boundary tests for every acknowledged commit — **process crash** (SIGKILL):
      `crash_oracle.py` asserts recovery ≥ last acknowledged commit − (N−1)
- [x] the same under **power loss** — `bench/powerloss/` (2026-09-24): a shim VFS keeps an undo
      log of every write since each file's last sync and, at a random I/O event, reverts the
      unsynced writes (all / random subset / torn at 512 B sectors) and dies.  285 runs:

      | synchronous | mode | runs | acknowledged commits lost |
      |---|---|---|---|
      | FULL | durable (`bf_deferred_commit` ≤ 1) | 60 + 45 sector-padded (`PL_PSOW=0`) | **never** |
      | FULL | durable, cycling ring (`bf_cache_size=262144`) | 60 | **never** |
      | FULL | `bf_deferred_commit=8` | 60 | 10 runs, ≤ 2 each — inside the 7-commit window |
      | NORMAL | durable | 60 | every run, up to 30 (NORMAL promises atomicity only) |

      Every run, in every cell, recovered an exact committed prefix with `integrity_check`
      ok, before and after a checkpoint: **no torn transaction and no corruption under any
      loss model**.  The oracle's durability bound is exact, not a heuristic: a commit counts
      as durable iff no file had an unsynced write when it was acknowledged (an earlier
      version counted SYNC lines and was fooled by SQLite's WAL-header sync).
      Not modelled: directory-entry durability (file create/delete) and drive write caches.
- [x] `PRAGMA synchronous` × mode matrix, process crash, 2026-09-24 (`crash_oracle.py` seeds
      101–112 × 4 recovery orders = 48 runs per cell, all 288 passing):

      | synchronous | `bf_deferred_commit` | runs that lost acknowledged commits | max lost |
      |---|---|---|---|
      | OFF / NORMAL / FULL | 1 | 0 / 0 / 0 of 48 | 0 |
      | OFF / NORMAL / FULL | 8 | 9 / 9 / 8 of 48 | 2 (window 7) |

      As a process crash must be, `synchronous` changes nothing; the deferral is the only
      source of loss, and it stays inside its window.  The realised window is smaller than N−1
      because any commit that also writes pages carries the batch with it.  Power-loss
      behaviour per cell is the open box above.

### H0 — make the comparison the paper's comparison
*From: parity plan Stage A (A1, A2, A4).*  **Mostly done already** — verified 2026-09-21, after
this plan initially carried the finished parts forward as open work.

- [x] budget arithmetic (A1) — `runner.py:222-270`, `split_memory()` rounds the ring **up** and
      raises the total, so `budget_bytes` is a floor and every SUT in a cell gets `ring +
      page_floor`
- [x] `paper` dataset — `bench/harness/configs/paper.json` (committed `b46a084`): 100 M records,
      `value_len = 16`, `theta = 0.9`
- [x] `--dist zipf-raw` — `bfbench.c:121` (`DIST_ZIPFRAW`), `:610`, `:1034` (output label),
      `:1193` (flag); both arms exist as `paper_raw` and `paper_scrambled`.  **Keep scrambled
      `zipf` equally reported** — YCSB scrambles, so ours is the more conservative question, and
      both belong in the thesis
- [x] per-record ring cost — `report.py:152` `bf_bytes_per_record`, `:154` `bf_records_per_page`,
      printed beside `cached_records` and `evictions` (`:333-338`)

Still open:

- [ ] **update and scan arms at the paper record shape.**  `paper.json`'s workload is
      `read=100` only; no config combines the paper dataset with an update or scan workload
- [ ] label single-threaded `paper` numbers a **floor** wherever they are reported: they omit
      the queueing effect that is part of why a hit is worth more with 30 readers in flight (S1)

The fairness rule that used to rest on this item now rests on **H2**: `paper.json`'s
`warmup_seconds` is still 420, so the shape is runnable but not yet trustworthy.

### H1a — permit `bf_min_record=4`
*From: handoff P3, parity plan C1.*  The size-class base **is already sweepable** —
`bfbench.c:421,878,1206` (`--min-record`), `runner.py:372-373`, and `configs/b1.json` is the
focused experiment.  One thing blocks the reference's own default.

- [ ] `src/bf_config.c:564` does `if( n < 8 ) n = 8;`.  The reference's default is **4**
      (`config.rs:27`), ours is 64, and §2.7 shows the relation is not monotonic — so the gap
      between them cannot be reasoned across, only measured, and today it cannot be measured
- [ ] verify the derived ladder is sane at base 4 (class count, the 4096 B ceiling,
      de-duplication of equal classes) before adding the axis
- [ ] add `bf_min_record` as an H3 axis including 4 and 64
- [ ] fix `src/bf_config.c:78-79`, which still carries the **retracted** claim that the
      reference's benchmark sets `cb_min_record_size` per workload.  It does not; nothing in
      `benchmark/` or `dev/` overrides the default.  The corrected version is already at
      `bf_config.c:51`

### H1b — forward `bf_copy_on_access` through the harness
Copy-on-access is the half that is genuinely unwired: `copy_on_access` appears **nowhere** in
`bfbench.c` or `runner.py`, so the mechanism landed in `c99c040` has never been swept and §2.4's
retention gap has no remedy measurement.

- [x] `--copy-on-access` in `bfbench.c`, forwarded from `runner.py`, echoed in the result
      JSON (`309ea3f`); liveness-checked: 0/10/40/100 move relocations, shed and evictions
- [x] axes taken from `benchmark/bench_bftree.toml` — `configs/h1b.json`, as two 1-D sweeps
      through the defaults (the cross product is >1 day at the steady-state warmup)
- [x] upgrade-shed as a SUT: `bf_shed` (`build_suts.sh --all`), crossed with the coa axis
- [x] ordering: **no constraint** — `bf_copy_on_access` is a global the ring re-reads on
      every (re)initialisation (`bf_circular_buffer.c:328`), so it survives a later
      `bf_cache_size`/`bf_min_record` rebuild; §3.4's claim that it is per-cache was wrong
- [x] run `configs/h1b.json` (54 runs, 169.6 min, 0 failed, clean `309ea3f`) and the per-seek
      confirmation `configs/h1b_seek.json` (clean `f08e09b`).  **DONE 2026-09-24; result below.**

**Result (v100, 4M rows, 32 MiB budget, zipf 0.9, 120 s warmup; every cell saturated,
evictions > 0).**  Hit rate is PER SEEK (`seek_served`/`seek_leaf`, `e153ea9`):

| copy_on_access | hit % / read | cached_records | evictions | model hit % |
|---|---|---|---|---|
| 0   | 46.8 | 87k | 93k–201k | 45.4 |
| 10  | 48.6 | 82k | 236k–241k | 47.9 |
| 20  | 48.9 | 77k | 259k–272k | 48.8 |
| 40  | 48.8 | 68k | 300k–347k | 49.6 |
| 100 | 40.2 | 24k | 1.1M–1.2M | 42.5 |

- "Model" = a record-level simulation of the same policy (FIFO ring, admission p=0.3 on miss,
  copy-on-access with the old slot left as dead space until the head passes, 88k slots).  The
  engine tracks it within ~1.5 points: **the implementation reaches its policy's ceiling.**
  The prediction in `h1b.json` holds — an interior optimum (20–40), and strict relocation
  (100) churns the ring into dead space (live `mini_page_bytes` 16.0 MB → 4.9 MB).
- **Throughput: no gain.**  h1b (within-campaign): stock 26,995 ops/s; bf 0.92–0.95x stock at
  coa 0–60 with coa=0 nominally best; 0.58x/0.75x at 80/100 (spreads up to 29%).  `bf_shed`
  is never better than `bf`.  Promotion axis: hit flat for promotion ≥ 5; promotion = 1 never
  saturated (0 evictions).  h1b_seek ran ~35k ops/s for the same cells: cross-campaign numbers
  are not comparable (powersave governor), as §6.5 says.
- **Metric correction (the bigger finding).**  `mini_page_hits/(hits+misses)` counts LOOKUPS:
  one point read makes several (descent shortcut, `RecordExists` at the leaf and in
  `fetchPayload`, each counted again by its `bf_btree.c` wrapper), so a miss counts ~1.8x and a
  hit ~1x.  Every record hit rate quoted before 2026-09-24 is 10–15 points low (h1b coa=0:
  32.7% by lookups, 46.8% per read) — including §2.4's "18–25 points below the Zipf ideal".
  `report.py` now uses the seek counters and suffixes old lookup ratios `L`.
- Also fixed: `bfbench.c` kept at most 40 `bf_cache_stats` rows while the pragma emits 43+,
  silently dropping `wal_commits` and `group_*` from every result JSON.
- **Verdict:** retention policy is not what holds reads back on this box.  Leave
  `bf_copy_on_access` at 10 (within noise of the optimum, matches the reference default).

### D3-core — let a buffered record shadow a same-key base cell
*From: perf plan Stage 3(c), and the precondition D3a and D3b both need.*  Gated behind V0 and
M1.

`src/btree.c:10593-10598` states today's invariant outright: *"Updates (loc==0) fall through to
the base write — this keeps a mini-page INSERT for a key that also exists in the base leaf
impossible, simplifying the merge rules."*  That single simplification is what blocks **both**
D3a and D3b, for different reasons, so it is lifted once rather than twice.

The reference has no such invariant: its insert is an **upsert** and a dirty INSERT simply wins
over the base record (`mini_page_op.rs:474`; see §4.4).

**DONE 2026-09-29.**  Most of it already existed: write-back delete (Stage 2.3) creates the
shadow state (delete then re-insert of a base row), so the forward/reverse merge picks
(`bfMergePick`: `insKey==baseKey` drops the base cell), `Count` (merged), the flush
(`bfApplyOneRecord` → `sqlite3BtreeInsert` overwrites) and replay were already upserts.  The
one hole was the **point seek**: its exact-match exit returned the BASE cell, so a write cursor
(UPDATE's `OP_Column` reads of the old row, and the index maintenance built from them) or any
seek on a single-leaf table (no descent ⇒ no shortcut) read the stale value.

- [x] exact-match exit asks `sqlite3BfBtreeKeyDirtyOp` and parks on a shadowing `BFOP_INSERT`
      (`btreeBfParkOnMini`, factored out of the not-found exit); counter `shadow_serves`
- [x] `bench/gen_update_stress.py` (6th generator in difftest/`stress_buf.sh`): checkpoints a
      populated base, then updates it — point/range/reverse/count reads, single-leaf table,
      overflow values, indexed column changes, index-driven multi-row UPDATE, delete/re-insert,
      REPLACE, rowid change, ROLLBACK, savepoints, mid-session checkpoints.  ~800
      `buffered_updates` and ~150 `shadow_serves` per script
- [x] **mutation test:** with the shadow serve compiled out, 109/162 difftest cases diverge
      (the new generator, `gen_stress` and `gen_rollback_stress`) — the oracle sees it
- [x] no ablation switch: it is correctness, needed with D3a off too (re-insert after delete)
- [x] full gate 50/50 (all four difftest variants incl. the cycling ring); SIGKILL after 400
      committed update transactions over a checkpointed base recovers byte-identical to stock
      under read-first and checkpoint-first, and stock reads the checkpointed file identically

Risk: high, and concentrated here rather than in the two items below.  Cursor invariants
(`BTCF_BfLeaf`, `BTCF_ValidNKey` — the Phase 1 scan bug lived here), splits, and the merge path,
which still needs the base page.

### D3a — ordinary existing-row UPDATE buffering
*From: handoff P4.*  Needs D3-core.  Win: WAL volume — updates emit `rec frames/commit = 0`
today and take the page-image path.

`src/btree.c:10577-10585`: when `loc==0` and the key length matches, the overwrite returns
through `btreeOverwriteCell()` **before** the BF block is reached.  Nothing about avoiding the
descent's leaf read changes that; this is a separate route into the buffer.

**DONE 2026-09-29 (correctness-gated; not yet measured by a campaign).**
- [x] representation: a `BFOP_INSERT` upsert — no new op, no payload version change; every
      consumer (log, replay, rehydrate, flush, checkpoint) already treats INSERT as upsert, and
      a mini-page overwrite clears the record's LOGGED bit so the new value is re-logged
- [x] route: the existing write-back-UPDATE block (rows living only in the mini-page, Phase 2)
      widened to a cursor on a base cell (`sqlite3BfBtreeUpdateCell`); behind
      `SQLITE_BF_NO_UPDATE_BUFFER`; counters `buffered_updates`, `update_fallbacks`
- [x] scope kept: SAVEPOSITION / preformat / nZero / value > `BF_MAX_MINI_PAGE` fall back to
      the base write (for a mini-page row: flush + re-seek first, as before); secondary indexes
      stay write-through
- [x] tripwire (NOT QUOTABLE; 1,000 single-row updates of a checkpointed 20k-row table):
      commits emit **1 record frame, 0 page frames** (stock: 1 page frame), so at group 1 the
      bytes per commit equal stock's (frames are fixed-size) and at group 32 the WAL holds
      ~1/28 of stock's frames before checkpoint.  The checkpoint then writes one page image per
      distinct dirty leaf (335 for those 1,000 updates).  `NO_UPDATE_BUFFER`: 531 page frames
      in a 500-update script vs 31 with buffering
- [x] differential (all variants), rollback, savepoint, recovery, checkpoint: see D3-core
- [ ] **measure** at the D gate: H6 (insert, ordinary update, mixed) with the switch as an axis

### D3b — blind INSERT: drop the uniqueness-probe leaf read
*From: perf plan Stage 3(a)(b)(d).*  The general path needs D3-core; the safe subsets named below
can be measured first without it.  Win: read I/O, not WAL volume — profile it separately and do
not let it claim D3a's result.

**Evidence, 2026-09-08:** every buffered insert faults in a 4 KiB base page — on btrfs, BF read
3.9 KiB/op against stock's ~100 B/op while writing 24x fewer bytes and running 31x slower
(2,028 vs 62,566 ops/s).  This is the largest expected win on real storage.

`sqlite3BtreeInsert` needs `loc`, `loc` comes from the descent, and *that descent is what reads
the leaf*.

**BUILT 2026-09-30 (correctness-gated; measurement is the D campaign).**  Design as landed:

- **Where the read is.**  Every explicit-key INSERT runs `OP_NotExists` first, and *that* seek
  reads the leaf.  For a plain INSERT its answer is required (a duplicate must fail).  For
  `INSERT OR REPLACE` on a rowid table with no index, trigger or FK, both of its exits are the
  next instruction, so the answer is irrelevant -- the reference's upsert.  `vdbe.c` detects
  exactly that shape (`p2 == pc+1`) and arms the cursor instead of seeking
  (`sqlite3BtreeBfArmBlind`); `sqlite3BtreeInsert` then descends only to the leaf's PARENT
  and buffers a `BFOP_INSERT` upsert against the child (D3-core makes that correct whether or
  not the key exists).  Anything declined seeks for real and continues as before.
- **Knowing the child is a leaf** without reading it: SQLite interior pages do not record
  height, so `BfCache.aLeafBit` is a bitmap of pages known to be non-root table leaves -- set
  when a descent enters one (`moveToChild`), cleared on free (`ForgetPage`), allocation,
  `xTruncate` and every rollback.  Roots are never recorded (`balance_deeper` changes a root's
  level in place).  A plain INSERT is never blind; neither the safe-subset idea (APPEND /
  PHANTOM) nor a full-page gap cache was needed.
- Switch `SQLITE_BF_NO_BLIND_INSERT`; counters `blind_inserts`, `blind_no_leaf`,
  `blind_refused`.  Known limitation: a ROLLBACK clears the whole bitmap (conservative), so a
  rollback-heavy workload goes blind rarely (crash test: 229 blind of ~2.5k REPLACEs at 10%
  rollbacks).  Tracking the pages a transaction allocated would make that exact.
- `bench/gen_blind_stress.py` (7th generator): page reuse (DROP/CREATE), `DELETE FROM`,
  VACUUM, splits under ROLLBACK/savepoints, root-leaf growth, auto-rowid after large blind
  keys, and **512/1024-byte pages** -- a stale bit is only observable at depth >= 3, and with 4
  KiB pages the generator could not see a bitmap that was never cleared (216/216 with every
  clear compiled out).  With small pages that mutant fails 108/216.
- Validation: full gate 50/50; SIGKILL after 500 committed REPLACE/DELETE/UPDATE transactions
  on a 512-byte-page tree recovers byte-identical to stock in both recovery orders.

**Seven older bugs this generator found (all reproduced with D3b compiled out, all fixed):**

| # | bug | symptom | fix |
|---|---|---|---|
| 1 | a root that was a leaf became interior in place (`balance_deeper`) keeping its mini-page | a deleted row served by a point read after the table shrank back | forget the root's mini-page there |
| 2 | `DELETE FROM t` zeroes the root in place | deleted rows served | forget the root after `clearDatabasePage` |
| 3 | `sqlite3BtreeTransferRow` bounds-checked a BF-served payload against the page | **every VACUUM (and xfer `INSERT…SELECT`) of a table with buffered rows: "malformed"** | skip the page bound for `bfOnMini`/`BTCF_BfLeaf` sources |
| 4 | VACUUM renumbers pages; the cache and the log kept old pgnos, and `CopyFile` commits via the pager, skipping the CLEAR hook | an extra row until reopen; after reopen 17 rows dumped twice | flush + stage CLEARs as VACUUM opens main's txn, clear the cache after the copy |
| 5 | ring: the head sweep reclaimed a FREELISTED block without unlinking it; once re-freed, two chains shared a node | segfault in `bfFreeListRemove` (cycling ring) | drop that size class's chain when the sweep reclaims a member |
| 6 | a dirty record larger than one WAL frame (small pages) cannot be logged | INSERT of a ~1.8 KB row at 512 B pages: "malformed" at commit | `bfDirtyRecordFits`: such records write through |
| 7 | `bfScanCanMerge` read `curIntKey` before the first descent -- it is UNINITIALISED | a merge scan armed on an INDEX cursor: covering-index `count(*)` "malformed" | test `pKeyInfo` only (also fixed in the new arm check) |

### M2 — full-page / gap cache (`BF_LOC_FULL`)
*From: parity plan B3 (corrected 2026-09-21), handoff P5.*  Currently **zero lines of logic**.
Its evaluation needs H2's trustworthy paper-shape arms: under scrambled zipf a full-page
promotion admits ~34 records to serve 1 and is ring waste; under contiguous zipf it can be a
large win.

Three corrections that change the design, all verified against `../bf-tree`:

1. **There is no 2 KiB rule.**  `DEFAULT_MAX_MINI_PAGE_SIZE = 2048` (`config.rs:24`) is marked
   `// Deprecated`.  The real bound is derived: `leaf_page_size - max_record_size_with_meta`,
   cache-line aligned down (`tree.rs:194-210`).
2. **On the read path it is an ALTERNATIVE to record caching, not an addition.**  `tree.rs:1419`:
   after the promotion dice, `if config.read_record_cache` insert `OpType::Cache`, **else**
   `upgrade_to_full_page`.  Turning it on turns record promotion off.
3. **On the write path it is a consequence of merging, not of a threshold.**  When a mini-page
   cannot grow it merges into base and is deallocated; *then*, if `write_load_full_page`
   (default true, `config.rs:155`) and the base page is non-empty, a full page is created
   (`mini_page_op.rs:938-956`).

The reference calls it a **gap cache, not a record cache** — "it caches the entire gap"
(`mini_page_op.rs:936`).

**2026-09-30 — measure first (owner decision).**  In this fork a full-page mirror in the ring
would duplicate SQLite's own page cache, which already holds whole leaves and answers absent
keys without I/O, and which the harness already gives part of BF's budget.  What Bf-Tree's
full page really buys is ONE pool in which pages and records compete.  So the question is
asked with existing knobs first: `configs/m2_split.json` moves a fixed 36 MiB between the
ring (2..32 MiB, new runner key `ring_bytes`) and the page cache, on scrambled and contiguous
zipf point reads and on contiguous range scans.  Page caching winning per byte on `zipfraw`
=> build M2 as a unified pool; a wash => document it as a non-transfer.
- [ ] run `configs/m2_split.json` (started 2026-09-30 from clean `aa1a49b`) and decide
- [ ] **M2a (write side):** mirror the leaf after a mini-page merges to base.  Additive.
      `SQLITE_BF_NO_FULL_PAGE`.
- [ ] **M2b (read side):** full-page promotion *instead of* record promotion.  A **mode** — the
      campaign treats it as a third arm, not a flag stacked on the others.
- [ ] copy-on-access and eviction for full pages
- [ ] report raw and scrambled zipf separately

### D4 — the 64 B write-amplification question, still open
*From: perf plan Stage 4.*  `pg frames/commit` regressed 1.00 → 3.74 at 64 B payloads (write
volume 182 → 924 MiB) when the derived size classes landed.  Two candidate causes were never
separated: larger mini-pages evict sooner (fewer fit in the ring), versus each eviction
merging into a base page that then goes to the WAL as a page image.

**Nobody has re-measured it** since the size classes became honest (`31d2a33`) and
`MiniPageCopy` stopped re-logging already-logged records.  It may be fixed.

- [ ] instrument merges and evictions per insert at 64 B; compare pre- and post-fix builds
- [ ] if it survives, the reference's answer is M2a — merge, then mirror the leaf as a full
      page — so fix it there rather than inventing a per-insert fallback

### M3 — separate scan promotion rate
*From: parity plan B4, handoff P6.*  `scan_promotion_rate` is distinct from
`read_promotion_rate` in the reference (`config.rs:38`).  One knob for both means scans either
thrash the ring or point reads under-admit.

- [ ] `PRAGMA bf_scan_promotion`

### M4 — eviction batching
*From: parity plan B5, handoff P6.*  **No longer blocked** — M1 landed, and the dirty head it
was supposed to be blocked by turns out not to arise on the write path at all.  Today: `sqlite3BfCacheEvict(pCache, 16)`
at three call sites — a fixed **16 entries**.  The reference accumulates toward
`TARGET_EVICT_SIZE = 1024` **bytes** with a retry cap of 10 (`tree.rs:1012-1018`).

- [ ] byte-target sweep + retry cap, `SQLITE_BF_NO_EVICT_BATCH`

### S1 — one shared record cache per database file, and threaded readers
*From: parity plan A3, handoff P7.*  **A project, larger than every M item put together.**
Do not mix it with M2–M4.

The reference runs 30 threads against **one** buffer pool.  N connections here allocate N
independent rings of `bf_cache_size` each (`bf_cache.c:227`), so threading the harness as-is
either overspends the budget N-fold or gives each thread 1/N of a cache — and the equal-budget
comparison is the foundation the harness rests on.

- [ ] **S1a (engine):** one cache per database file, refcounted lifecycle
- [ ] lock the **28 `sqlite3BfBtree*` entry points** (71 call sites in `btree.c`), not the leaf
      functions — locking the leaves would give false confidence, because the API hands out raw
      pointers into cache state (`sqlite3BfMapLookup` returns a `BfMapEntry*`) and the lock must
      span the *caller's* use of them
- [ ] design pinning / stable handles for the two merge scans, which retain mini-page pointers
      **across** btree steps
- [ ] make REF marking and map-directory mutation safe: `sqlite3BfMiniPageSearch` sets REF with
      a read-modify-write on `valueLenAndRef` — the same `u16` carrying the value length and
      the WAL-logged flag, so a lost update corrupts a length, not a hint — and
      `sqlite3BfMapLookup` walks a directory that `bfMapEnsureCapacity` reallocs under it
- [ ] **S1b (harness, independent — can land first):** `bfbench --threads N` with per-thread
      connection, prepared statements, RNG and histograms; zipf table and Feistel permutation
      shared read-only; histograms merged at the end; writers keep serialising via
      `busy_timeout` (a documented non-transfer)
- [ ] 1→30 reader scaling, one total memory budget

Note: `BfCache.mutex` exists and is **never entered** (§3.3).  Treat it as absent.

### M1 — flush-capable dirty eviction — **DONE 2026-09-21, and it barely matters**
*From: handoff P2, where it was ranked the largest remaining parity gap.  The measurement
demoted it; see below.*

- [x] evict from a btree-owned, re-entrancy-safe context that has table/root information —
      `sqlite3BfBtreeRelieveEvictStall()`, drained from `sqlite3BtreeInsert`
- [x] stage pending record batches before materialising (inherited: `bfFlushOneMiniPage`
      calls `sqlite3BfBtreeGroupStagePending` first)
- [x] merge dirty records into the base page — via `bfFlushTableDirty` on the stalled leaf's
      own root, so splits and restarts are handled by the path that already does this
- [x] counters + `SQLITE_BF_NO_DIRTY_EVICT`
- [x] 256 KiB-ring differential gate
- [ ] crash tests at the merge boundary — **still open**, folded into D1

**What it does.**  `evictCallback` cannot flush: no `Btree`, no write transaction, and it runs
inside the allocator.  So it now records the leaf that aborted the sweep in
`pgnoEvictStall`, and `sqlite3BtreeInsert` drains it — flushing the stalled leaf's table,
re-seeking (mandatory: the flush replays through its own cursor and can rebalance) and
retrying the buffered insert once. Guarded on `rootPgno>1` as corruption containment; WAL
payload v2 now restores the root during recovery (§3.2, D1).

**Why it barely matters, measured.**  60 k inserts in one transaction, 256 KiB ring,
100 B values:

| | 1 table | 12 tables interleaved |
|---|---|---|
| evictions | 3,360 | 4,379 |
| `evict_stall_seen` | **0** | 79 |
| `dirty_evict_flush` | 0 | 25 |
| `dirty_evict_retry` | 0 | **2** |
| buffered inserts | 58,235 | 58,244 |

Two inserts rescued out of 58,244, and on the single-table path the stall **never occurs**.
The reason was already in the tree: when BF refuses an insert, `btree.c` falls through to
`sqlite3BfBtreeFlushTableForMutation`, which flushes that table's ENTIRE dirty set, so the
FIFO head is clean before the next sweep reaches it.  The stall survives only ACROSS tables,
because that fallback flushes just the cursor's own root.  **The item's premise — "a dirty
head stops the sweep" — is true of `evictCallback`'s code and false of the running system.**

**Then it lost a row, and that turned out to be the point of the exercise.**  A cross-table
differential against stock (12 tables, one transaction, 256 KiB ring) came back one `INSERT`
short.  With `SQLITE_BF_NO_DIRTY_EVICT` the same workload was byte-identical.  The trigger was
exact: the loss happened only where the stalled leaf equalled the leaf the cursor was about to
buffer into.  The cause was **not in M1** — it was `evictCallback` ignoring `pEvictProtect`
(§2.5), a defect M1's flush-then-buffer sequence is the only known way to reach.  So M1's real
contribution was as a detector.

M1 now also refuses to flush the cursor's own leaf.  With the root cause fixed that guard is an
efficiency choice, not a correctness one — the workload is byte-identical either way — but
flushing a leaf you are about to re-dirty is self-defeating, so it stays.  It also costs M1 its
only rescues: `dirty_evict_retry` goes from 2 to **0**.

**Kept, at the owner's decision, with its value stated honestly**: it is small,
off-switchable, discharges M4's stated dependency, and it found a real bug.  Its measured
effect on throughput or capacity is **zero**, and it must never be reported as anything else.

**Two process notes, because both cost time and both were caught by counters rather than
reasoning.**  (1) The first overflow test used `zeroblob(100)`, which sets `nZero` and leaves
`nData` at 0 — and the buffering gate requires `nData>0 && nZero==0`.  `buffered_inserts` came
back 0: it measured a workload where BF never engaged.  A write test that does not check
`buffered_inserts` is not testing BF.  (2) The first honest run showed 52 flushes for 4
rescues; the natural diagnosis ("the head holds a RUN of dirty slabs") was wrong, and
flushing the whole table instead of one leaf changed the numbers by exactly zero, which is
what falsified it.  The real cause was that `pgnoEvictStall` was sticky — set by one sweep,
consumed by a later unrelated refusal (a mini-page already at the maximum size class, which
no flush can fix).  Scoping the flag to its own sweep (`sqlite3BfCacheEvict` clears it on
entry) took 52/4 down to an honest 25/2.

---

### Measurement milestones

These are tier-2 (§6.1) and run at the named campaign gates below, not per change.

* **H2 — warmup proof at the exact `paper` shape.**  The current 420 s default is unproven;
  do not transfer `steady`'s measured 900 s floor to this different shape.  Establish a floor
  separately for `paper_raw` and `paper_scrambled`, preferably in admitted operations, using
  `cached_records` plateau or sustained cycling.  Required outputs per cell: `cached_records`,
  `evictions`, live mini-pages, records/mini-page, bytes/record, record and page hit rates,
  throughput and block read bytes.
* **H3 — the end-of-stage ablation campaign.**  One matrix: `bf_min_record` (4, 64, justified
  candidates) × copy-on-access ratio × read promotion × upgrade shed × eventually full-page
  mode, scan promotion, eviction batching × raw and scrambled zipf × saturated and
  larger-than-memory.
* **H4 — conservative steady-state baseline.**  `configs/steady.json` on final code, ≥3
  repeats, labelled accurately: single-threaded, buffered I/O, 100 B values, scrambled zipf,
  cgroup-constrained larger-than-memory.
* **H5 — reference-shaped campaign.**  `configs/paper.json` after H2: 100 M records, 16 B
  values, ~1 GiB budget, zipf 0.9, raw and scrambled arms, plus update and scan arms at the
  same record shape.  Before S1 this is a single-threaded buffered-I/O approximation and must
  say so.
* **H6 — write/durability campaign.**  Insert, ordinary update, mixed; auto-checkpoint 0 and
  normal; `synchronous` off/normal/full; grouping 1/8/32 or whatever D2 replaces it with.
  **The strict `group_commit=1` arm is required, not optional**: per-transaction cost is the
  number a durability comparison needs, and §2.3 shows we currently quote only the grouped one,
  whose denominator is WAL commits rather than transactions.  Report `group_deferred` beside
  every frames-per-commit figure, and rename `report.py`'s `rec frames/commit` column to say
  which denominator it uses.
  Report separately: WAL growth per commit, total block-layer write bytes per commit, record
  frames/commit, page frames/commit, checkpoint traffic, and the acknowledged-commit guarantee.

### Definition of done — named in advance so it is not argued backwards

1. **Reproduces.**  Paper config + honoured budget + N threads shows a clear win.  `steady`
   then documents sensitivity to record size and access clustering.  Best outcome, most
   defensible claim.
2. **Reproduces partially**, residual traceable to buffered I/O.  Quantify it — bytes avoided
   vs. time saved is already instrumented — and state direct I/O as the known, deliberately
   unported prerequisite.
3. **Does not reproduce at parity of configuration.**  Only then is "structural difference from
   SQLite's B-tree" honest, and it needs a code-level diff against `../bf-tree` behind it.

## 6. Validation and measurement

### 6.1 Two tiers, and mixing them up is the error

**Tier 1 — per change, correctness only.**

```sh
sh bench/stress.sh                         # differential oracle vs sqlite3_stock
sh bench/stress_buf.sh                     # ... vs the buffering build
BF_GROUP=8 sh bench/stress_buf.sh          # ... with group commit on
BF_PROMOTION=100 sh bench/stress_buf.sh    # ... with read promotion at max
BF_CACHE_SIZE=262144 sh bench/stress_buf.sh  # ... with a ring small enough to CYCLE
sh bench/stress_xtable.sh                  # MANY tables in one txn, ring too small
```

Run the `BF_CACHE_SIZE` variant.  At the default ring these workloads never evict — every
benchmark in this repo reports `evictions=0` — so eviction, free-list reuse and
upgrade-under-pressure went untested for the project's whole life.

**Run `stress_xtable.sh` too, for a different structural reason.**  Every other generator
drives ONE table, and with one table the insert fallback flushes that table on the first BF
refusal — so a foreign table's dirty mini-page can never be at the ring head, and a whole
set of states is unreachable no matter how many seeds you run.  Gate anything touching
eviction, the mapping table or the size-class ladder on it.  It does **not** cover the
`pEvictProtect` bug in §2.5; its header says so.  That is how a segfault
in `bfFreeListRemove` (`bench/ring_repro.sh`) reached a commit.  At 262144 a single suite run
produces ~2,200 evictions, ~10,300 upgrades and ~1,400 compactions.

`configs/fixes_smoke.json` may be run as a **tripwire** only.  Its verdicts are "nothing
exploded" and "stop, something did".  **Its numbers are never quotable** — no warmup,
`cached_records` still climbing.

**Tier 2 — at a named campaign gate, one campaign with an ablation axis.**  A steady-state read
campaign costs hours (`steady`'s larger-than-memory arm needs a 900 s warmup per cell), and a
campaign gate covers several mechanisms; paying that per change serialises the work behind the
measurement.
One campaign at the gate, sweeping the switches, attributes each mechanism *within* the
campaign, which is the only kind of comparison this repo trusts — identical code has measured
9913 / 11652 / 8102 ops/s across campaigns.

The accepted cost: a regression is found later, with more changes between it and the last
known-good point.  The ablation axis is what keeps that bisectable, which is why the switch is
a hard requirement (§1 rule 5) and not a nicety.

### 6.1b The gate is one parallel command (2026-09-24)

`python3 bench/gate.py` (quick, ~1 min) and `--full` (~3–4 min) run every checker —
`difftest.py` (stress + stress_buf × {base, ring, group8, promo100} + xtable, 1,149 cases at
full seeds), the crash oracle sharded by seed (default, ring, group 8, promotion 100,
`SQLITE_DEBUG`), the power-loss oracle, the repro scripts and the codec test — on all cores,
against a snapshot of `build/` in a tmpfs dir.  The same work took about two hours through the
sequential shell scripts, and forbade touching `build/` or `bench/` while it ran.  Where the
time went, measured: one test at a time on 12 cores (≈10×); btrfs-on-LUKS fsyncs under the
default `synchronous=FULL` (2.75× per test — power loss has its own oracle now); stock
recomputed per knob variant although it ignores the BF pragmas (1.6×).

**Determinism is part of the gate.**  Read promotion drew from `sqlite3_randomness`, seeded by
the OS, so one script promoted different rows on every run: a real bug appeared in 9 of 40 runs
of the same script and could not be minimised.  Promotion now uses a per-cache xorshift with a
fixed seed (`promoteRng`, `bf_cache.h`); any failure the gate reports reproduces exactly.

### 6.2 The debug build is the fastest diagnostic here

```sh
cd build && cc -O0 -g -DSQLITE_DEBUG -DSQLITE_BF_INSERT_BUFFERING \
   -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE -I. -I../src \
   -o sqlite3_dbg shell.c sqlite3.c -lm -lz
python3 ../bench/gen_stress.py 7 wal 2500 > /tmp/d.sql && ./sqlite3_dbg /tmp/d.db < /tmp/d.sql
```

`SQLITE_DEBUG` turns on hundreds of internal invariant checks.  The 2026-09-15 wal-index
corruption bug fired `walIndexAppend`'s own assert immediately, where the release build
produced "database disk image is malformed" several statements later in a different
operation.  Reach for this before printf.

Do **not** add `-DSQLITE_OMIT_SHARED_CACHE` reflexively: it silences the table-lock assert,
which is a real check on BF's own cursors — that assert is what pointed at
`bfFlushOneMiniPage` opening a write cursor without the lock the VDBE would have taken.
A debug build is far slower; use it for correctness, never for timing.

### 6.3 The performance loop (measure, don't guess)

It exists because guessing cost us once: a "10x read regression" turned out to be the *write*
path — the benchmark's temp-table load — masquerading as reads.

1. **Isolate ONE path.**  Split phases and time them separately.  If a read benchmark
   contains inserts, it is a write benchmark.  Put helper data in a separate ATTACHed
   database so the measured run does no writes at all.
2. **Profile before touching code.**
   ```sh
   cc -O2 -g -fno-omit-frame-pointer -DSQLITE_BF_INSERT_BUFFERING ... -o sqlite3_bf_prof
   perf record -q --call-graph fp -F 999 -o p.data -- ./sqlite3_bf_prof db < w.sql
   perf script -i p.data | python3 bench/tools/flamegraph.py out.svg "title"
   ```
   `bench/tools/flamegraph.py` is self-contained; it prints a self-time table, which is what
   you act on.  `kernel.perf_event_paranoid=2`: user-space sampling works, kernel tracing
   does not.  `valgrind --tool=callgrind` gives deterministic counts when sampling is noisy.
3. **Fix the frame the data names, then RE-PROFILE.**  Our first `IsDirty` fix looked
   obviously right and left the function at 82% of samples, because the hot case was CLEAN
   mini-pages that scan to the end.  The real fix was an O(1) flag.
4. **Re-run the oracles.**  Several of these touch dirty tracking, where a wrong answer
   silently loses writes.
5. **Check occupancy (`cached_records / live_mini_pages`) before optimising any per-record
   loop.**  That one division would have killed two of the 2026-09-15 reverts in advance.
6. **Stop when what remains is structural** — documented design overhead, not an accident.

**Be suspicious of every regression.**  The paper reports wins on every metric.  If we
measure a slowdown, the null hypothesis is that OUR integration is wrong.  Cross-check
`../bf-tree/` (`src/tree.rs`, `mini_page_op.rs`, `nodes/`, `circular_buffer/`, `range_scan.rs`,
`wal/`, and `benchmark/`).  Only after showing our code matches theirs is "structural
difference" an honest conclusion — and check the CALL GRAPH, not just that the cited line says
what it is said to say: `Consolidate` was cited correctly and was still unreachable.

### 6.4 The full correctness arsenal, and what each one is for

The paper itself validates with differential fuzzing against a reference model, libFuzzer +
ASan, and a static mini-page reference model.  We mirror it and extend:

* **Differential oracle — the top gate.**  `stress.sh` / `stress_buf.sh` + generators;
  byte-identical `.out` and `.dump` against a `-DSQLITE_OMIT_BF_CACHE` stock build.  Runs on
  every change.  **Its structural blind spot:** an optimisation that silently does nothing is
  still *correct*, so the oracles cannot see a dead mechanism.  That is why every mechanism
  ships a counter (§1 rule 5) — `BF_COPY_REFERENCED` and `sqlite3BfMiniPageConsolidate` were
  dead for the project's whole life and 864 oracle runs never noticed.
* **Crash-differential oracle — `bench/crash_oracle.py` (2026-09-24).**  Random multi-table
  workloads (one table created mid-stream, one with a secondary index, overflow values,
  rolled-back transactions); stock computes the state hash after every commit; BF is
  SIGKILLed after a random acknowledged commit; recovery must equal *some* committed prefix at
  or after the last acknowledgement (minus the group window under `BF_GROUP`) and pass
  `integrity_check`, under four recovery orders including a crash **during** the checkpoint;
  then the remaining transactions must reach stock's final state.  Found five of the six bugs
  in D1 on its first runs.  Its limit: SIGKILL keeps the OS page cache, so it tests process
  crashes, not power loss.
* **Power-loss oracle — `bench/powerloss/` (2026-09-24).**  `plvfs.c` (a shim VFS + SQL
  driver) undoes unsynced writes at a random I/O event — all, a random subset, or torn at
  sector granularity — and dies; `powerloss_oracle.py` judges recovery against stock's prefix
  hashes: an exact committed prefix, `integrity_check` ok, and at least every commit
  acknowledged while nothing was unsynced (minus the deferral window).  Results in D2.
* **Deterministic crash reproducers.**  `bench/recover_repro.sh` (root-pgno recovery, 3 orders),
  `bench/torn_tail_repro.sh` (torn record-frame tail), `bench/ckpt_repro.sh`.
* **Full-ROLLBACK generator.**  `bench/gen_rollback_stress.py`, the fifth generator in
  `stress_buf.sh`: the only one that issues full ROLLBACK, empty and failed transactions and
  failing statements.  The other four never reach `sqlite3BtreeRollback` with committed records
  buffered, which is how bug D1-1 lived.
* **Fuzzing** (`.claude/agents/libfuzzer-tester.md`).  clang libFuzzer + ASan/UBSan on (a) the
  record-frame decoder and recovery path fed malformed WALs — must reject or recover, never
  UB — and (b) SQL with BF on, following `test/dbfuzz2.c` / `test/ossfuzz.c`.
* **ESBMC bounded model checking** (`.claude/agents/esbmc-verifier.md`) on the pure,
  pointer-bounded modules: record-frame **encode∘decode = identity** plus bounds safety on
  truncated input; mini-page insert offset-frontier non-overlap and binary-search correctness;
  circular-buffer head/tail state-machine invariants; **checkpoint/recovery idempotence**
  (replay twice = replay once).  Keep harnesses small — the amalgamation is out of reach.
* **Property-based / model-based testing.**  The generators already track a live row dict;
  assert it against the engine over random op sequences.  Plus C-level PBT on the mini-page
  and WAL-codec modules.

Both testing agents are scoped to *our* changes, not SQLite core, which is already trusted.

### 6.5 Campaign hygiene

* **Never touch shared state during a gate.**  `bench/` and `build/` are frozen while a suite
  runs; it reads binaries, work dir and generators per test.  Two false "corruption" failures
  came from ignoring this.
* **Get the pragma order right** — see the three rules in §3.4.  `bf_cache` before any pager,
  then `journal_mode`, then the per-cache knobs, with `bf_min_record` after `bf_cache_size`.
  Every one of them fails silently.
* **Keep scratch databases off tmpfs.**  The first reading of the truncate collapse was taken
  on tmpfs where I/O is free (24x); on btrfs the same sweep is 2,028 → 1,368 ops/s.  Both are
  true and they answer different questions — state the storage with every write number.
* **Read the counters before the wall clock.**  `pg frames/commit`, `buffered%`, `evictions`,
  `ring MiB`, read/write bytes are governor-independent; the governor here is `powersave`.
* **Steady state means a plateau or sustained cycling.**  Agreement between equally
  under-warmed repeats is not evidence: every repeat warms for the same duration, so two runs
  that are both too short agree perfectly.

### 6.6 Every thesis-grade campaign archives

`manifest.json`, `results.jsonl`, the generated `RESULTS.md`, the source revision and
amalgamation/build hash, dirty status, the exact config, and machine / filesystem / governor /
cgroup metadata.  At the time of writing, `bench/harness/results/` holds no auditable final
bundle.

## 7. Out of scope, and why

### 7.1 Deliberately not ported

* **Direct I/O / io_uring backends** (`StdDirect`, `IoUringPolling`, `IoUringBlocking`).
  SQLite's unix VFS does not use `O_DIRECT`, and the comparison this thesis makes is against
  gold-standard SQLite; porting it would change what we compare against.  It remains the
  honest prerequisite for claiming the paper's absolute numbers, and it is why both
  cache-retention experiments failed to pay for themselves: under buffered I/O an avoided
  miss is usually an OS page-cache hit.  Keep it as a sensitivity experiment, not a
  prerequisite.
* **CPR snapshots** (`src/snapshot.rs`).  SQLite has its own durability model; the
  record-granular physiological WAL is the deliberate replacement.
* **Write concurrency.**  `BtShared` serialization stays.  Read concurrency is in scope (S1).
* **Native mini/base leaf split.**  SQLite materialises fixed pages and runs its own
  balancing; see §7.3.
* **Secondary-index record buffering** — *deferred, not rejected.*  v1 scope keeps indexes
  write-through so a committing transaction still emits one ordered WAL stream with one commit
  marker, which makes atomicity trivial.  It becomes a candidate phase of its own **if** the
  measurement campaign shows the primary-table win is large enough to be worth re-opening the
  atomicity question.

### 7.2 Multi-writer, and what it would buy — **parked, revisit only after everything else**

Group commit does not transfer to this fork, and the reason is SQLite's, not ours:
`sqlite3WalBeginWriteTransaction` (`wal.c`) takes an exclusive `WAL_WRITE_LOCK` — *"Only one
writer allowed at a time"*.  WAL mode gives reader/writer concurrency, not writer/writer.  The
lock is held for the whole transaction, so a second writer cannot even produce its records
until the first has committed; there is never more than one transaction at the commit-flush
point, and a flush with nothing to amortise against is just an fsync.

**With serialized writers, "batch across transactions" and "wait until durable" are mutually
exclusive.**  That is why the current `bf_group_commit` defers rather than waits: deferral is
the only batching available to a single-writer engine, and it buys throughput with durability.

**The enabler exists**: SQLite's `begin-concurrent` branch (the basis of Expensify's Bedrock)
lets write transactions run concurrently and validates page-level conflicts at COMMIT.  Bodies
overlap, commits still serialize — which is exactly what group commit needs.

**And there is a genuine contribution hiding in it.**  `BEGIN CONCURRENT` detects conflicts at
PAGE granularity, so two transactions inserting different rowids into the same leaf conflict
although they do not overlap logically.  BF does not modify the page; it appends to a mini-page
delta, and two disjoint record sets on one leaf merge deterministically.  So:

> Bf-Tree's mini-page representation lowers SQLite's optimistic-concurrency conflict detection
> from page granularity to record granularity, converting a class of false conflicts into
> successful concurrent commits.

That is testable, novel, and a property of THIS fork rather than of the paper.

**Cost, ranked.**  (1) fix the recovery defect first — concurrency over an unsound recovery
path is not worth building; (2) merge `begin-concurrent` into a fork that has already rewritten
`wal.c` and `pager.c`, the same files it touches; (3) **S1 becomes mandatory**, because the
record cache is per pager and a record buffered in connection A is invisible to B; (4)
record-level conflict detection, including replay ordering for interleaved transactions; (5)
group commit itself, days once the rest exists.  Multi-month; out of scope for the TFG as
implementation, strong as a Future Work chapter.

**Before any of it, one cheap experiment bounds the whole thing**: group commit can only ever
recover the fsync cost of a commit, so measure insert throughput at `synchronous=off` versus
`full` at `bf_group_commit=0`.  That gap is the entire prize.

### 7.3 The faithful branch (after this one is measured)

This fork keeps SQLite's fixed-page `btree.c` and inserts Bf-Tree as a record cache *beside*
it: pragmatic, incrementally correct, and oracle-friendly.  It therefore leaves performance on
the table — the larger-than-memory win comes from **memory density**, and a fixed-page pcache
that also pins full 4 KiB pages for the same leaves dilutes exactly that advantage.

The reference is variable-size and record-granular natively: a logical leaf resolves through
the page table to `PageLocation::{Mini, Full, Base, Null}`, where all three are the *same*
`LeafNode` at different `node_size` (up to `MAX_LEAF_PAGE_SIZE = 32 KB`).

**Plan:** once this fork is complete and measured, branch a file-incompatible, maximally
faithful variant — variable-size leaves as the on-disk unit, mini-page-native navigation,
density-first buffer pool, direct I/O as a first-class mode — and evaluate the two
head-to-head.  The goal is to quantify **the cost of SQLite-shaped integration**, reporting
resident bytes (mini vs full), point/scan/write throughput and write amplification for both.
Its correctness can no longer lean on the byte-identical `.dump` oracle; use the model-based
oracle instead.

---

## 8. Reference sources

* Paper: Hao & Chandramouli, *Bf-Tree: A Modern Read-Write-Optimized Concurrent
  Larger-Than-Memory Range Index*, PVLDB 17(11), 2024 —
  https://vldb.org/pvldb/vol17/p3442-hao.pdf.  Durability §5.7; eviction/copy-on-access §5.2;
  range scan §5.3; negative search §5.6; copy-on-access ratio Figure 14.
* Reference implementation: `../bf-tree/` (github.com/microsoft/bf-tree), plus
  `doc/snapshot-recovery.md` and github.com/XiangpengHao/bf-tree-docs.
* Prior integration fork: `../sqlite`, branch `ablation-phase0` — knowledge, not code.
* In-repo: `BF_TREE_V2_KNOWLEDGE.md`, `docs/reference/BF_TREE_DESIGN.md` (§7/§13 durability),
  `bench/harness/README.md`.

---

## 9. Progress log

Append newest-last.  Do not rewrite old entries; append corrections and mark superseded
conclusions explicitly.

```text
### YYYY-MM-DD HH:MM — agent/model — short task title

- Starting revision/state:
- Goal:
- Evidence gathered:
- Changes made:
- Files changed:
- Validation commands and exact outcomes:
- Benchmark status: NOT RUN / NOT QUOTABLE / publishable artifact path
- Decisions and rationale:
- Remaining blockers or uncertainty:
- Exact next action:
```

### 2026-09-21 — Zed GPT-5.6 — baseline optimization/parity audit, handoff created

- Starting revision/state: `main` at `54f4ae6`; 12 modified tracked files, untracked `tfg/`.
- Goal: summarize optimization state, verify parity claims against source and `../bf-tree`,
  prioritize code and benchmark work, create a persistent cross-agent handoff.
- Evidence gathered: reviewed the knowledge/plan/parity documents, harness README and configs,
  the diff and recent history; compared core mechanisms with `../bf-tree`; verified source
  behaviour for copy-on-access, dirty eviction, grouping, recovery comments and checkpoint
  materialisation.
- Changes made: created `BF_TREE_V2_AGENT_HANDOFF.md`; added it to `CLAUDE.md` required reading.
- Validation: none — documentation only.  The source working tree remained unvalidated.
- Benchmark status: NOT RUN.
- Decisions: dirty eviction and durability/recovery ambiguity rank above further retention
  micro-optimisation; harness exposure of B1/B2 is the next low-risk measurement-enabling work;
  shared cache/threading stays a separate large project.
- Exact next action: run the differential and cycling-ring validation on the working tree.

### 2026-09-21 — Claude Opus 5 (+ Sonnet fact-check) — four planning documents merged into one

- Starting revision/state: `main` at `b0026ba`; dirty tree +22/−653 across 12 files (the
  dead-code sweep); untracked `tfg/` and `BF_TREE_V2_AGENT_HANDOFF.md`.
- Goal: correct the stale state section, and collapse `BF_TREE_V2_PLAN.md`,
  `BF_TREE_V2_PARITY_PLAN.md`, `BF_TREE_V2_PERF_PLAN.md` and `BF_TREE_V2_AGENT_HANDOFF.md`
  into this single document.
- Evidence gathered: a second model (Sonnet) ran an independent source-level fact-check of all
  four documents in parallel; findings folded in below.  Verified directly: HEAD and the shape
  of the dirty diff; `runner.py:222-270` `split_memory()` (A1 done); `bf_btree.c:583-589`
  (group commit stages nothing for the first N−1 commits); the ablation switches present in
  `src/`; `bf_config.c:564` clamping `bf_min_record` below 8.
- Corrections the fact-check produced, now folded in:
  - the previous handoff recorded HEAD as `54f4ae6` and called the free-list class-selection
    fix the pending behavioural change; that fix is **committed** (`b0026ba`), and the current
    dirty tree is pure dead-code removal with no behavioural change intended;
  - eviction batching: current behaviour is `sqlite3BfCacheEvict(pCache, 16)` — a fixed **16
    entries** at three call sites, not "one block per call"; the reference's target is ~1024
    **bytes**;
  - `BfCache.mutex` is allocated but `bfCacheEnter`/`bfCacheLeave` have **zero call sites** —
    the parity plan's "Done:" bullet reads as though locking exists.  Recorded as scaffold;
  - `BF_LOC_FULL` has zero producers — the full-page cache is absent, not scaffolding;
  - `src/bf_config.c:78-79` still carries the claim the parity plan retracted on 2026-09-21
    (that the reference's benchmark sets `cb_min_record_size` per workload).  Filed under H1a;
  - Stage A (workload shape, per-record ring cost) was not represented in the handoff's P-list
    at all; it is now **H0**, and its premise — no Stage-B feature is evaluable before the
    comparison is fair — is stated as an ordering rule in §5.
- Changes made: this file; deleted the four merged documents; updated `CLAUDE.md` required
  reading to point here.
- Validation commands and exact outcomes: none — documentation only.  **The working tree is
  still ungated: V0 is untouched.**
- Benchmark status: NOT RUN.
- Decisions and rationale:
  - `BF_TREE_V2_KNOWLEDGE.md` stays separate: it is the map, not a plan, and it is read first.
  - The backlog is renumbered into one V/D/M/H/S sequence rather than preserving three
    parallel numbering schemes (Phase 0-4, Stage A-D, and the handoff's own P- and B-lists),
    which is what let the same
    item exist three times with three statuses.
  - `bf_group_commit` is recorded as a **contradiction** between §4.2's locked decision and
    `bf_btree.c:583-589`, not merely as an open item, because the locked decision is what the
    thesis comparison rests on.
- Remaining blockers or uncertainty:
  - the dirty tree has not been built or gated (V0);
  - D1's `BF_FULL`-plus-checkpoint interaction still has no correctness proof;
  - the durability contract for `bf_group_commit` is unchosen (D2);
  - the crash-injection oracle's current pass/fail state is unknown — nothing was run.
- Review pass (second model, after the merge): restored the B2 revert numbers (+1.2 pts hit
  / −3.6% ops), the perf plan's still-open 64 B write-amplification question (now **D4**) and
  the deferred secondary-index buffering note; corrected `bf_promotion` to its real name
  `bf_promotion_rate` and added the missing `bf_cache` pragma (`tool/mkpragmatab.tcl:410-436`).
  It independently re-verified `btree.c:10604`, `runner.py:222-270`, `bf_btree.c:583-589`, the
  three `sqlite3BfCacheEvict(pCache, 16)` call sites, the zero `bfCacheEnter` call sites, the
  absent `BF_LOC_FULL` producers, and every `../bf-tree` citation in §4.4.
- Note for whoever reads this next: the old `BF_TREE_V2_AGENT_HANDOFF.md` was **untracked**, so
  it has no git object and its original text is unrecoverable.  Its content lives on in §1, §2,
  §3, §5 and this log; its `*From: handoff P*` provenance tags cannot be checked against a
  source any more.
- Exact next action: **V0**.  Build, run the four differential oracle variants plus the
  `BF_CACHE_SIZE=262144` cycling-ring run and `bench/ring_repro.sh` under `SQLITE_DEBUG`,
  record the exact outcomes here, and commit the dead-code sweep or fix what it broke.

### 2026-09-21 — Zed GPT-5.6 — merged-plan review opened for agent discussion

- Starting revision/state: `main` at `b0026ba`; existing dead-code sweep, plan consolidation,
  benchmark warmup edit and untracked `tfg/` remain present.
- Goal: review the merged canonical plan for internal consistency and source/harness parity,
  without silently changing disputed direction.
- Evidence gathered: checked the merged plan against `src/btree.c`'s update/insert paths,
  `bench/harness/configs/paper.json`, `bfbench.c`, `runner.py`, `report.py`, the harness PRAGMA
  ordering documentation, current Git status and WAL counter definitions.
- Changes made: created `BF_TREE_V2_PLAN_REVIEW.md`, a temporary discussion record with twelve
  source-backed review items, response fields for Claude, and final-decision fields reserved for
  the user.  The canonical backlog has not been changed pending agreement and user decisions.
- Files changed: `BF_TREE_V2_PLAN_REVIEW.md`, this progress-log entry.
- Validation commands and exact outcomes: documentation review only; no build or test run.  V0
  remains ungated.
- Benchmark status: NOT RUN.
- Decisions and rationale: the one-plan consolidation remains the recommended structure; disputed
  content should be resolved explicitly rather than creating another competing plan.
- Remaining blockers or uncertainty: Claude has not responded to R1-R12; the user has not made
  final decisions; source working tree still needs V0.
- Exact next action: Claude reviews `BF_TREE_V2_PLAN_REVIEW.md`, records AGREE/PARTIAL/DISAGREE
  with evidence for each item, then the user decides the final direction before accepted edits are
  applied to this canonical plan.

### 2026-09-21 — Claude Opus 5 + Zed GPT-5.6 — plan review R1–R12, user decisions applied

- Starting revision/state: `main` at `b0026ba`; the merged plan as first written; Zed's review
  in `BF_TREE_V2_PLAN_REVIEW.md` raising twelve items.
- Goal: reach an agreed direction on all twelve, with the user deciding every disputed point.
- Evidence: each item verified against source before responding; full verdicts and reasoning are
  in `BF_TREE_V2_PLAN_REVIEW.md`.  Ten of twelve accepted as Zed wrote them; two amended.
- **Corrections to the merged plan's own errors, all mine:**
  - §2.2 claimed Phase 2 does "write-back insert/**update**/delete".  It does not
    (`btree.c:10577-10585`).  The §2.2 row and `CLAUDE.md` both said so while §3.2 said the
    opposite, in the same document.
  - H0 listed work that was already finished and committed (`paper.json` at `b46a084`,
    `zipf-raw`, the `report.py` columns).  I carried the old parity plan's A2 forward without
    checking — exactly what rule 2 forbids.
  - H1 said neither retention mechanism could be swept.  `bf_min_record` has been sweepable
    since `a2fcde3`; only copy-on-access is unwired.
  - "BF pragmas after `journal_mode`" is wrong for `bf_cache`, the one global pragma, and
    omits a third rule (`bf_min_record` after `bf_cache_size`).  Getting the first backwards
    produces a run with no record cache that still reports plausible numbers.
  - The 900 s warmup floor was stated as if it were shape-independent.  `steady.json`'s own
    comment says the fill is driven by operations, not wall clock.
  - "The environment, not the code" asserted a negative about our own code that the open
    parity gaps make untestable, and contradicted the project's standing null hypothesis.
- **Two places where Zed was wrong, both corrected in the review file rather than applied:**
  - R7 claimed this worktree changed `steady.json`'s warmup 420→900.  It did not — the file
    already carried `warmup_seconds: 900`; the only edit was a stale document reference inside
    a comment array.
  - R8 argued the "1.00 record frame per commit" and "~30x less WAL at group 32" figures must
    come from different arms.  They do not: `results/sweeps/results.jsonl`, experiment
    `group_commit`, has both in one grouped run (`wal_record_frames = wal_commits = 983`).
    The inference failed because `wal_commits` counts WAL commit *events*, not SQL
    transactions — a deferred transaction never reaches `sqlite3WalFrames`
    (`bf_btree.c:583-590`, `pager.c:6684-6688`).  **But the row was still misleading**, for a
    reason Zed did not name: 983 WAL commits absorbed 30,720 transactions, so the figure is
    ~1 frame per 31 commits, and the metric silently embeds D2's deferral window.  Fixed with
    the denominator made explicit, plus a required strict arm in H6.
- **User decisions:** R1 — shared prerequisite then split (D3-core → D3a/D3b).  R5 — keep
  Phase 2 `DONE` with the UPDATE caveat and D1/D2 pointers, not a demotion.  R10 — label §4.3
  as target, defer the architecture choice to D1's evidence.  Remaining nine applied as
  amended.
- Changes made: §2.1 restructured into four categories (R7); §2.2 Phase 2 row (R5); §2.3
  results row, provenance and denominator (R8) and warmup qualification (R6); §2.4 re-headed
  and re-worded (R9); §3.4 three ordering rules (R4); §4.3 labelled target + D1 checklist item
  (R10); D3 split into D3-core / D3a / D3b (R1); H0 marked mostly done (R2); H1 split into
  H1a / H1b (R3); "letter" replaced by named gates (R11); H6 gains the required strict arm.
- Files changed: `BF_TREE_V2_PLAN.md`, `BF_TREE_V2_PLAN_REVIEW.md`.
- Validation commands and exact outcomes: none — documentation only.  **V0 still untouched.**
- Benchmark status: NOT RUN.
- Remaining blockers: unchanged — V0, D1, D2, and now the checkpoint-architecture decision
  D1 must inform.
- Exact next action: **V0**, unchanged.

### 2026-09-21 — correction to the merge entry above (rule 11: append, do not rewrite)

The entry titled *"four planning documents merged into one"* says "deleted the four merged
documents".  In git terms that is wrong about three of the four: `BF_TREE_V2_PLAN.md` was
**rewritten in place** (`M`, not `D` then `A`), `BF_TREE_V2_PARITY_PLAN.md` and
`BF_TREE_V2_PERF_PLAN.md` were removed with `git rm`, and `BF_TREE_V2_AGENT_HANDOFF.md` was
**untracked** and simply deleted.  The distinction matters for recovery: the two companion
plans are in history and can be read back with `git show HEAD:<file>`; the handoff **has no git
object and cannot be recovered**.  Its content survives only in §1, §2, §3, §5 and this log.

### 2026-09-21 — Zed GPT-5.6 — final pre-implementation consistency pass

- Starting revision/state: `main` at `b0026ba`; source dead-code sweep remains ungated; merged
  plan and review resolutions are present in the working tree.
- Goal: perform the final consistency review before V0 implementation begins and preserve the
  user's final authority over disputed direction.
- Evidence gathered: reviewed Claude's R1-R12 responses, their application to this plan, current
  Git status, the grouped-WAL archived-run explanation, H2 wording, D3 dependencies and remaining
  old-stage terminology.
- Changes made:
  - corrected residual stale text: H2 no longer transfers `steady`'s 900 s floor to the paper
    shape; H0/H2 and M2 dependencies now name the actual measurement gate; obsolete stage-boundary
    wording now names H3/H4/H5/H6;
  - clarified that D3b's safe subsets can be measured before D3-core while its general path
    requires D3-core;
  - changed V0's final step from an unconditional commit to preparing the cleanup and committing
    only on explicit user request;
  - changed `BF_TREE_V2_PLAN_REVIEW.md` from “user-decided” to **agent consensus awaiting explicit
    user ratification**.  The prior progress entry's “user decisions” wording was premature; no
    item-by-item user decision was present in the conversation available to Zed.
- Files changed: `BF_TREE_V2_PLAN.md`, `BF_TREE_V2_PLAN_REVIEW.md`.
- Validation commands and exact outcomes: documentation consistency only; `git diff --check`
  pending immediately after this entry.  V0 source validation has not started.
- Benchmark status: NOT RUN.
- Decisions and rationale: Claude's R8 correction is accepted — 1.00 is per WAL commit event, and
  983 WAL commits absorbed 30,720 SQL transactions.  The denominator fix in §2.3 is correct.
  The R1 shared-prerequisite split and R10 evidence-gated checkpoint decision are technically sound
  agent consensus, but remain the user's call.
- Remaining blocker: explicit user ratification of the proposed R1-R12 resolution set.
- Exact next action: user ratifies or amends the consensus; then mark the review resolved and begin
  V0 exactly as listed, with no mechanism implementation before the gate passes.

### 2026-09-21 — user ratification recorded

- Decision: the user explicitly replied **“approved”**, ratifying the complete R1-R12 resolution
  set as amended by the final consistency pass.
- Changes made: `BF_TREE_V2_PLAN_REVIEW.md` marked resolved and user-ratified; provisional labels
  removed.  No source implementation changed.
- Validation commands and exact outcomes: documentation-only; final `git diff --check` follows
  this entry.  V0 source validation remains not run.
- Benchmark status: NOT RUN.
- Exact next action: begin **V0**.  No D/M/H/S implementation starts until the dead-code sweep
  builds and passes the listed differential, cycling-ring and debug gates.

### 2026-09-21 — Claude Opus 5 — V0 gated and committed; M1 built, measured, demoted

- Starting revision/state: `main` at `b0026ba`, dirty dead-code sweep, nothing built or gated.
- Goal: close V0, then implement M1 (flush-capable dirty eviction).
- **V0.** Rebuilt `sqlite3`, `sqlite3_buf`, `sqlite3_stock` first — the on-disk binaries
  predated the sweep.  Results in the V0 entry above.  `BF_GROUP=8` was stopped at 27/216
  (0 failures) and `BF_PROMOTION=100` never ran, both at the owner's instruction to stop
  waiting; recorded as owed rather than claimed.  Committed as `f5f09dd` (sweep) and
  `bd338af` (plan consolidation).  See the process note in V0 about the commit authorisation.
- **M1.** Implemented: `evictCallback` records the leaf that aborted the sweep
  (`pgnoEvictStall`); `sqlite3BfBtreeRelieveEvictStall()` drains it from `sqlite3BtreeInsert`,
  flushing that leaf's table, re-seeking and retrying once; guarded on `rootPgno>1`; behind
  `SQLITE_BF_NO_DIRTY_EVICT`; four counters in `PRAGMA bf_cache_stats`
  (`evict_stall_seen`, `dirty_evict_flush`, `dirty_evict_retry`, `dirty_evict_refused`).
- **The result is negative and it re-ranks the backlog.**  2 inserts rescued out of 58,244;
  on a single-table workload the dirty-head stall never occurs at all, because the existing
  fallback (`sqlite3BfBtreeFlushTableForMutation`) flushes the whole table on the first
  refusal.  M1's premise was a true statement about `evictCallback`'s code and a false
  statement about the running system.  Kept — it is small, switchable, closes a real
  cross-table hole and discharges M4's dependency — but demoted out of the active ranking.
- **Three things the counters caught that reasoning did not**, all worth remembering:
  - a write test using `zeroblob(100)` buffers NOTHING (`nZero` set, `nData` 0, and the gate
    needs `nData>0 && nZero==0`).  `buffered_inserts` is the check that a write test is
    testing BF at all;
  - the obvious diagnosis for 52 flushes / 4 rescues ("the head holds a run of dirty slabs")
    was wrong, and flushing the whole table instead of one leaf moved the numbers by exactly
    zero, which is what disproved it;
  - the real cause was a sticky `pgnoEvictStall` — set by one sweep, consumed by a later
    unrelated refusal that no flush could help.  Scoping it to its own sweep gave 25/2.
- Files changed: `src/bf_cache.{c,h}`, `src/bf_btree.c`, `src/btree.c`, `src/bf_config.c`,
  `BF_TREE_V2_PLAN.md`.
- Validation: `stress_buf.sh` and `BF_CACHE_SIZE=262144 stress_buf.sh` running on the M1 tree
  at the time of writing; functional counter checks done.  **M1 is NOT committed.**
- Benchmark status: NOT RUN (the numbers above are counters, not throughput).
- Remaining blockers: D1's crash tests at the merge boundary now also cover M1's flush point;
  the two owed V0 runs.
- Exact next action: finish the M1 gate, then **H1b** — wire `bf_copy_on_access` through
  `bfbench.c`/`runner.py`, the cheapest unmeasured thing left.

### 2026-09-21 — Claude Opus 5 — M1 landed; it found a latent data-loss defect

- Starting state: `main` at `bd338af`, M1 built and counter-instrumented, gates clean.
- **A cross-table differential lost a row.**  12 tables, one transaction, 256 KiB ring: one
  `INSERT` absent from the BF dump.  `SQLITE_BF_NO_DIRTY_EVICT` made it identical to stock, so
  the ablation switch §1 rule 5 demands paid for itself within an hour of the mechanism
  existing.
- **Root cause, and it is not M1's**: `evictCallback` never checked `pEvictProtect` (§2.5).
  Diagnosed by trace, not by reading: four rounds — the retry path, the cursor's leaf identity,
  the flush target, and finally a read-back after every buffered insert, which showed
  `sqlite3BfRecordWrite` returning `BF_OK` and `sqlite3BfRecordRead` returning `BF_NOT_FOUND`
  for the same leaf and key one instruction apart.  `-DSQLITE_BF_NO_MINIPAGE_COMPACT` made the
  loss vanish, localising it to the evict-and-retry paths.
- **A claim I made and then had to retract**: that the defect is reachable without M1.  It is
  not demonstrated.  A fix-reverted binary passes the original repro, all of
  `stress_xtable.sh`, and a read-then-write workload built to produce the state, so long as
  M1's same-leaf guard is present.  Recorded as unproven in §2.5; the fix is defensive.
- Changes: `evictCallback` refuses a protected slab; both `pEntry->pPage = pNew` sites now
  restore `locType` too; M1 gained a same-leaf guard (efficiency, not correctness); new
  `bench/stress_xtable.sh` + `gen_xtable.py`.
- **The new oracle does NOT catch this bug**, and both files say so in their headers.  It
  covers the cross-table shape, which the single-table generators structurally cannot reach.
  Shipping it as a regression test for the defect would have been a test that cannot fail.
- Files changed: `src/bf_cache.{c,h}`, `src/bf_btree.c`, `src/btree.c`, `src/bf_config.c`,
  `bench/stress_xtable.sh`, `bench/gen_xtable.py`, `BF_TREE_V2_PLAN.md`.
- Validation: `stress.sh` 54/54, `stress_buf.sh` 216/216, `BF_CACHE_SIZE=262144 stress_buf.sh`
  216/216, `stress_xtable.sh` 3/3 — all ALL CLEAN on the combined tree.
- Benchmark status: NOT RUN.  The M1 figures in its entry are counters, not throughput.
- Decisions: owner kept M1 after the re-rank; committed on an explicit instruction.
- Remaining blockers: **D2 needs an owner ruling** on the durability contract before anything
  can be built there.  D1's crash tests now also owe coverage of M1's flush point.
- Exact next action: **D1** — recovery/checkpoint ownership, the highest-ranked unblocked item.

### 2026-09-21 — Claude Opus 5 — D1 opened; crash recovery corrupts the B-tree

- Starting state: `main` at `c8e7046`, M1 and the `pEvictProtect` fix committed.
- Goal: D1, the highest-ranked unblocked item — prove or disprove the recovery/checkpoint path.
- Built `bench/recover_repro.sh`: SIGKILL after a committed transaction with
  `wal_autocheckpoint=0`, then three checkpoint orders (descend-first, checkpoint-first,
  checkpoint-in-a-separate-process), each gated on row count **and** `integrity_check`.
- **Finding: BF crash recovery leaves a structurally corrupt B-tree** —
  *"Tree 2 page 2 cell 24: Child page depth differs"*, in all three orders.  See D1 above for
  the three controls that make it ours and narrow it to the recovery path.
- The hole this probe was written for — the documented leaf→root `rootPgno` gap — turns out
  **not** to be the defect: no rows are lost in any order.  Worth recording, because that gap
  has been carried as "future work" since Phase 2 and the real problem was elsewhere.
- **A flaw in my own first version, worth keeping as a lesson**: it gated on row count only,
  printed `ALL CLEAN`, and displayed the corruption in passing.  A test that reports success
  while showing corruption is worse than no test.  Both halves gate now.
- Files changed: `bench/recover_repro.sh` (new), `BF_TREE_V2_PLAN.md`.
- Validation: the probe itself; stock control clean in all three; clean-close control clean.
- Benchmark status: NOT RUN.
- Exact next action: root-cause the corruption.  First moves: run the probe under a
  `SQLITE_DEBUG` build (the project's fastest diagnostic — the 2026-09-15 wal-index bug fired
  `walIndexAppend`'s own assert immediately); compare the base tree's page structure before the
  crash and after recovery; and check whether a crash with BF's insert buffering compiled out
  (`-DSQLITE_OMIT_BF_CACHE` is not enough — use the Phase-1 `bf_ro` shape) still corrupts,
  which separates the record-WAL from the flush path.

### 2026-09-22 — Zed GPT-5.6 — root-cause and fix crash-recovery tree corruption

- Starting revision/state: `wal-commit-frame-merge` at `b2292d3`; recovery trace preserved in
  `26a72a1`; untracked `tfg/` left untouched.
- Goal: reproduce, diagnose and fix the top-priority durability blocker in
  `BF_TREE_V2_RECOVERY_BUG.md`.
- Evidence gathered: reproduced all three failures at 250 and 2000 rows. The 250-row trace
  showed one recovered mini-page with `owner=5 root=5`, 49 genuinely absent records, and the
  transition from `rowid=245 leaf=5 depth=0 nPage=5` to
  `rowid=246 leaf=7 depth=1 nPage=7`. The committed WAL had correct `nTruncate=5` and page
  images 1–5. Thus duplicate replay and page-count mismatch were ruled out.
- Root cause: WAL v1 omitted the table root. Recovery defaulted `rootPgno` to the leaf owner,
  so checkpoint opened a write cursor on leaf 5 as a root. Its first split made page 5 an
  interior root over pages 6/7 while page 5 remained a child of the real root page 2.
- Changes made: bumped the BF payload to version 2; encoded/decoded/indexed `rootPgno`; logged
  each mini-page's owning root; restored it during cache replay; updated codec tests and the
  stale recovery-gap documentation.
- Files changed: `src/bf_wal.{c,h}`, `src/bf_btree.c`, `src/bf_cache.c`, `src/wal.c`,
  `src/pager.c`, `test/bf/wal_codec_test.c`, `BF_TREE_V2_RECOVERY_BUG.md`,
  `BF_TREE_V2_{KNOWLEDGE,PLAN}.md`, `CLAUDE.md`, `.claude/agents/esbmc-verifier.md`. `pager.c`
  only gained the missing forward declaration needed to compile the branch with `SQLITE_DEBUG`.
- Validation commands and exact outcomes: standalone `wal_codec_test`: `ALL PASS`; `make
  sqlite3`: success; `recover_repro.sh` at 250, 2000 and 4000 rows: all 3 scenarios pass at
  every size; `SQLITE_DEBUG` recovery at 250 rows: all 3 pass with no assertion; default
  `stress_buf.sh`: timed out at 10 minutes after at least 65 consecutive passes and no failure;
  `BF_CACHE_SIZE=262144 sh stress_buf.sh 1`: `ALL CLEAN` 12/12; `stress_xtable.sh`: `ALL
  CLEAN` 15/15.
- Benchmark status: NOT RUN; correctness-only change, no throughput number quotable.
- Decisions and rationale: persist the owning root in the physiological record instead of
  inferring it from mutable leaf identity or waiting for a query descent. File compatibility is
  intentionally not required, and a version bump makes old payloads fail closed.
- Remaining blockers or uncertainty: D1 still owes BF_FULL, multiple-root and crash-during-
  checkpoint coverage; D2's acknowledged-commit contract remains open.
- Exact next action: run the differential, small-ring and crash gates, then continue D1 edge
  coverage.

### 2026-09-24 — Claude Opus 5.5 — D1: a crash-differential oracle, and eight bugs it and a ROLLBACK generator found

- Starting state: `main` at `df84190`, clean except untracked `tfg/`.  All binaries on disk
  predated `df84190`'s merge or were built before it; rebuilt everything first (stale binaries
  have produced false findings here before).
- Goal: the previous entry's next action — gate the root-persistence fix, then D1 edge coverage
  (multiple roots, crash during checkpoint, `BF_FULL`).
- Built `bench/crash_oracle.py`: random 40–100-transaction workloads over four tables (one
  created mid-stream, one with a secondary index, overflow values, rolled-back transactions);
  stock hashes the logical state after every commit; BF is SIGKILLed after a random
  acknowledged commit and must recover to a committed prefix at or after it (minus the group
  window under `BF_GROUP`), pass `integrity_check`, and then reach stock's final state; four
  recovery orders including a SIGKILL *during* the checkpoint.  Verified to have teeth: the
  session-start binary fails 8/8 runs of the first two seeds.
- **It found, on its first run, that the BF load itself fails with "malformed" — no crash
  needed.**  Minimised to: committed buffered rows, then an empty `BEGIN;ROLLBACK`, then any
  read → the rows are gone, and gone for good after a clean close.  `sqlite3BtreeRollback`
  emptied the record cache, which since Phase 2 holds committed rows that exist only there and
  in the WAL.  Every ROLLBACK, including a failed COMMIT and an autocommit statement failing a
  constraint.  The four differential generators never issue a full ROLLBACK (only
  `ROLLBACK TO`), so this lived for the project's whole Phase 2.
- The complete list, with reproducers and fixes, is in D1 (bugs 1–8).  In brief: rollback
  destroyed committed rows (1, and 7 via a transaction-level `ROLLBACK TO`); recovery re-applied
  ops a flush had already put in base (2); recovery replayed torn commits (3); group commit tore
  transactions that also wrote pages (4); a torn tail starting with a record frame made the next
  write drop its own wal-index entry (5); reverse range seeks skipped buffered rows (6); an
  index-driven same-size UPDATE of a buffered row returned SQLITE_CORRUPT (8).  Each reproducer
  fails on the session-start binary and passes on the fixed one.
- Design decisions:
  * **Replay order is logged, not inferred.**  WAL payload v3 adds `BFWAL_OP_CLEAR`; a flush's
    commit logs one per flushed leaf ahead of its own records; replay starts each leaf after its
    last CLEAR.  My first design inferred supersession from page images (plus a per-leaf flush
    before in-place overwrites and at commit to keep that exact).  The oracles broke it twice —
    the per-leaf flush rebalanced dirty siblings (scan returned rows out of order), and even
    table-wide, a flush lands a leaf's records on whatever leaf holds their keys now, so a
    newer image of the *original* leaf may never exist.  With CLEAR, an op no CLEAR covers was
    never applied to base and no page-path write can have touched its key, so the extra flushes
    were removed again — which in turn exposed bug 8, which they had been masking.
  * **Rollback rebuilds the cache from the log** (WAL ops, then staged batches, then the open
    group batch) instead of emptying it.  The alternative — preserving committed records across
    a rollback in place — needs undo information the mini-pages do not keep.
  * These correctness mechanisms have counters but **no off switch**; §3.2 says why.
- Files changed: `src/{bf_btree.c, bf_cache.c, bf_cache.h, bf_config.c, bf_wal.c, bf_wal.h,
  btree.c, pager.c, pager.h, pcache.c, pcache.h, wal.c, wal.h}`, `test/bf/wal_codec_test.c`;
  new `bench/crash_oracle.py`, `bench/gen_rollback_stress.py` (fifth `stress_buf.sh` generator),
  `bench/torn_tail_repro.sh`; `bench/stress_buf.sh`, `bench/recover_repro.sh` (stale header);
  `BF_TREE_V2_PLAN.md`, `BF_TREE_V2_KNOWLEDGE.md`, `CLAUDE.md`.
- Validation, on the final source (amalgamation byte-identical to the one tested in scratch):
  `wal_codec_test` ALL PASS (new CLEAR cases); `torn_tail_repro.sh` PASS release and debug, and
  FAIL on both the session-start binary and a build with only the wal-index probe removed;
  `recover_repro.sh` 250/2000 rows and 250 under `SQLITE_DEBUG`: ALL CLEAN; `ckpt_repro.sh` OK;
  `stress_xtable.sh` ALL CLEAN 15/15; `gen_rollback_stress` sweep 0/72 diverged (12 seeds × 3
  journal modes × release+debug), and 0/32 with top-level savepoints added (session-start binary:
  17/32); crash oracle 612/612 across six configurations — default 160, `bf_cache_size=262144`
  120, `BF_GROUP=8` 160, `SQLITE_DEBUG` 32, `bf_promotion_rate=100` 80, 100-txn histories with
  ring + `BF_GROUP=4` 60.  `stress.sh` ALL CLEAN 54/54.  **At commit time (owner asked to
  commit before the gate finished) `stress_buf.sh` was at 65/65 passing, and its
  `BF_CACHE_SIZE`/`BF_GROUP=8`/`BF_PROMOTION=100` variants and `ring_repro.sh` had not run yet;**
  their results follow in the next entry.
- Benchmark status: NOT RUN.  Correctness-only change (§6.1).  Expected costs, all unmeasured:
  a write-transaction rollback replays the WAL's ops; each flush adds ~11 B of CLEAR per leaf
  to its commit; group commit no longer defers commits that write pages, so group-mode WAL
  savings on mixed workloads will shrink; `walFrames` reads a few wal-index slots per call.
- Remaining and owed: the `integrity_check`-inside-a-transaction false positive (D1, pre-existing,
  not fixed); a reproducer for `replay_dropped`>0 (`BF_FULL` during replay loses the record —
  the old comment claiming otherwise was false); a power-loss crash VFS (SIGKILL cannot test
  fsync ordering, which D2 needs); the checkpoint-architecture decision (D1, the user's).
- Exact next action: fold the top-level savepoint shape into `bench/gen_rollback_stress.py`
  (validated in scratch, above) once the gate releases `bench/`; then **commit, on the owner's
  go-ahead** — the change is uncommitted, and until it lands every ROLLBACK on `main` can lose
  committed rows.

### 2026-09-24 (cont.) — Claude Opus 5.5 — D2 settled: durable mode named, verified under power loss

- Starting state: branch `d1-rollback-replay-clear` at `d0f3904` (the D1 commit, made on the
  owner's instruction while the gate was still running; gate results below).
- Goal: D2 — what an acknowledged commit guarantees.
- Evidence: a survey of `../bf-tree` (D2 section): its committer waits until its record is
  `pwrite()`n in a ~1 ms shared batch, never fsyncs, and its WAL is off in every benchmark;
  its recovery is incomplete.  Tripwire WAL-volume measurements (NOT QUOTABLE) showed the
  whole group-mode saving is the deferral itself.
- **Owner decision (AskUserQuestion, 2026-09-24): "name it honestly".**  Implemented:
  `PRAGMA bf_deferred_commit` is the canonical name (`tool/mkpragmatab.tcl`), `bf_group_commit`
  kept as an alias; the comments that called the deferral "the paper-faithful setting"
  (`bf_config.c`, `bf_btree.c`) now say the opposite and state the N−1 window; §4.2 and
  `CLAUDE.md` amended.  No behaviour change.
- Validation: process-crash matrix `synchronous` {OFF, NORMAL, FULL} × mode {1, 8}, 288/288,
  loss only in deferred mode and within its window (D2 table).  Built the power-loss oracle
  (`bench/powerloss/plvfs.c`, `powerloss_oracle.py`); 285/285 across five configurations (D2
  table).  Oracle teeth: NORMAL loses acknowledged commits in every run, as it may.
  First-version oracle defect found and fixed: it took SQLite's WAL-header sync as covering
  commit 0.
- Benchmark status: NOT RUN.
- D1 gate on `d0f3904` (for the record the previous entry promised): see the next entry.
- Remaining: none of D2's boxes; drive write caches and directory fsync are out of the
  oracle's model.  §2.3's "~30× less WAL" is a deferred-durability number and must be
  labelled so wherever it is reported (harness report labels still owed).

### 2026-09-24 (cont.) — Claude Opus 5.5 — the gate in minutes, and three bugs the speed exposed

- Owner asked why the suite is slow.  Measured (above, §6.1b) and rebuilt: `bench/difftest.py`
  (parallel, tmpfs, binary snapshot, stock once per script) and `bench/gate.py` (every checker,
  two tiers).  Full gate: 50/50 tasks green in **~3.5 min**; quick tier **~50 s**.  The old
  scripts stay as the reference definitions; `recover_repro.sh`'s cleanup glob was narrowed so
  concurrent runs cannot delete each other's files.
- **Found by the first fast runs, all fixed:**
  1. *My own D1 regression* — rollback rehydration replayed the group-commit batch and then
     reset it, so a SECOND rollback before any commit lost deferred-committed rows
     (`bf_deferred_commit=8`, `gen_rollback_stress`).  Fixed with a carry list on the group
     (`struct BfGroup`): replayed by every rollback, staged ahead of the open batch, never
     consumed by a rollback; the deferral count is preserved so the N−1 window still holds.
  2. *Pre-existing lost update* — the write-through cache refresh returned early for a value
     too big for a mini-page (or refused), leaving the previously PROMOTED clean copy to serve
     the old value.  Now it drops the leaf's clean records (`nStaleCacheDrop`).  Repro: an
     index-driven UPDATE to an ~6 KB value after the row was read and promoted.
  3. *Nondeterministic promotion* (above) — which is why (2) survived: it needed a particular
     promotion pattern and appeared in ~22% of runs.
- `gen_rollback_stress` now also issues top-level `SAVEPOINT`s (bug D1-7's shape).
- Validation: full gate green on the final build (difftest 1,149/1,149; crash oracle 1,320
  runs; power-loss 240 runs; repros; codec; ring_repro).
- Benchmark status: NOT RUN.  Note for the campaign: promotion is now deterministic, so repeat
  runs of a benchmark cell are no longer independent draws of the promotion coin.

### 2026-09-24 (cont.) — Claude Opus 5.5 — H1b swept; the record hit rate was mis-measured

- Ran `configs/h1b.json` (54 runs, 0 failed) and `configs/h1b_seek.json`; results, table and
  verdict in the H1b item.  Copy-on-access: interior hit-rate optimum at 20–40, zero
  throughput gain; the engine matches a record-level simulation of its policy within ~1.5 pts.
- Found and fixed (`e153ea9`): the record hit rate counted lookups, not reads (a miss ~1.8x),
  understating every hit rate ever quoted by 10–15 points; new `seek_served`/`seek_leaf`
  counters, report.py switched.  And bfbench dropped stats rows past 40.
- Validation: quick gate green on `e153ea9` (12/12).  Numbers: within-campaign only.
- Exact next action: **D3-core** (let a buffered record shadow a same-key base cell), then D3a.

### 2026-09-29 — Claude Opus 5.5 — D3-core + D3a: existing-row UPDATE is buffered

- Committed first: H1b write-up (`f9c0e91`).
- D3-core turned out to be one hole, not a merge rewrite: shadowing already existed (write-back
  delete + re-insert) and the scans/flush/replay handled it; the point seek's exact-match exit
  did not.  Fixed (`btreeBfParkOnMini` + `sqlite3BfBtreeKeyDirtyOp`), and D3a widened the
  existing write-back-UPDATE block to base cells behind `SQLITE_BF_NO_UPDATE_BUFFER`.
- New generator `gen_update_stress.py`; a mutation test (shadow serve compiled out) fails
  109/162 cases, so the oracle can see the state it targets.
- Validation: full gate 50/50 on the final build; the `NO_UPDATE_BUFFER` build 648/648 on
  difftest base+ring; SIGKILL/recovery byte-identical to stock in two recovery orders.
- Numbers: tripwire only (above, NOT QUOTABLE).  Group 1 gains no bytes (one fixed-size frame
  per commit either way); the WAL-volume win needs group commit, as D2 says for inserts.
- Exact next action: the D-letter campaign (H6-style write arms: insert / update / mixed,
  group 1 and 32, `bf` vs a `NO_UPDATE_BUFFER` SUT), or D3b (blind insert) first if the owner
  prefers to batch the write mechanisms into one campaign.

### 2026-09-30 — Claude Opus 5.5 — D3b blind insert built; its generator found seven older bugs

- Owner: "build d3b first then benchmark".  D3b as designed in its item (REPLACE-shaped
  inserts skip `OP_NotExists`'s leaf read via a known-leaf bitmap).
- `bench/gen_blind_stress.py` exposed seven bugs that predate D3b (table in D3b); the two worst:
  every VACUUM of a table with buffered rows failed, and a ring free-list corruption segfault.
  Also learnt: a generator must be proven able to see its target (mutation test) -- the first
  version could not, until it used small pages; and a reducer's temp dir must be on tmpfs
  (rollback-journal fsyncs on btrfs-on-LUKS timed every run out, reading as "no divergence").
- Harness: `bfbench --insert-mode replace`, runner key `insert_mode`, SUTs `bf_noupd` /
  `bf_noblind` in `build_suts.sh --all`.
- Validation: full gate 50/50; mutation test (bitmap clears compiled out) 108/216 fail; crash
  test byte-identical to stock.
- Exact next action: the D-letter write campaign (`configs/d3.json`).

### 2026-09-30 (cont.) — Claude Opus 5.5 — D campaign: the write buffer cannot batch at this shape

- Ran `configs/d3.json` from clean `30c05a1` (44 runs, 85 min, 0 failed; a first attempt died
  at 9/44 in a shutdown and was discarded, not resumed).  v100 4M rows, 32 MiB budget, zipf 0.9,
  synchronous=NORMAL, WAL, harness default `wal_autocheckpoint` (SQLite's 1000 frames).
- **Mechanisms are live**: 61-68% of REPLACEs went blind; 350-430k buffered UPDATEs per cell;
  ablation SUTs show 0 for their counters.
- **No throughput win, and the reason is measured, not guessed**: `clear_logged` ~= ops in every
  insert/replace/update cell -- nearly every buffered record is materialised into its base leaf
  ALONE.  Between two auto-checkpoints (~1000 frames) ~1000 random records land on ~1000 of
  ~120k leaves, so each checkpoint flush reads and rewrites one leaf per record.  D3b's skipped
  leaf read is paid at flush time instead (replace read B/op: bf 5.8k vs bf_noblind 5.3k at
  group 1); D3a adds a record frame and still pays the page (update write B/op at group 1:
  bf 10.8k vs bf_noupd 7.7k; 0.92x vs 1.02x of stock).  Under UPDATE the ring also fills with
  dirty records (164k-378k evict stalls, 14-31% of updates fall back to base writes).
- Cells, ops/s vs stock (within-campaign; spreads in results/d3/RESULTS.md): insert bf 1.00 /
  0.73 (group 1/32, the 32 cell 68% spread); replace bf 1.01 / 0.98, noblind 1.00 / 1.17;
  update bf 0.92 / 0.97, noupd 1.02 / 1.02; mixed bf 0.87 / 0.88, noupd 0.91 / 0.95.  One real
  BF advantage: insert p99 latency 407 us vs stock 1018 us at group 1.
- Verdict: at this data:ring:checkpoint shape D3a/D3b MOVE work rather than remove it.  The
  lever is the checkpoint policy (the D1 checkpoint-architecture decision, owner's): records must
  live long enough to share a leaf when materialised.  Next measurement: an autocheckpoint axis
  (0 / 1000 / 10000 / 100000) on update and replace, bf vs ablation, reporting records per
  flushed leaf (ops / clear_logged) beside ops/s.

### 2026-09-30 (cont.) — Claude Opus 5.5 — M2 started as a measurement

- Owner: "let's work on m2", then chose *measure first* over a faithful port / unified pool /
  skip.  Reason recorded in the M2 item: a ring full page duplicates SQLite's pcache here.
- `runner.py` gains `ring_bytes` (explicit ring, pcache = rest of the same total);
  `configs/m2_split.json` (36 runs, ~2 h) launched from clean `aa1a49b`.
- The `d3_ckpt` campaign died with the previous session at 9/36 and was NOT restarted (it would
  have competed with M2 work); rerun it on an idle machine before the checkpoint decision.
