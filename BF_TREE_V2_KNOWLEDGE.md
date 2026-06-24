# BF-Tree v2 — Knowledge Transfer

> Everything an implementer (human or model) must know before building the new fork.
> Companion to `BF_TREE_V2_PLAN.md` (the staged plan). This document is the *map*; the plan
> is the *route*. Read both before writing code.
>
> Source material: the VLDB'24 paper (Hao & Chandramouli, *Bf-Tree*, PVLDB 17(11)), the Rust
> reference impl `github.com/microsoft/bf-tree`, and ~2 weeks of a prior C integration in this
> repo (`ablation-phase0`, modules under `src/bf_*.c`). Line numbers below drift — verify
> against `src/` before relying on them.

---

## 0. One-paragraph orientation

Bf-Tree is a B-tree whose hot data is cached and buffered at **record granularity** in
variable-size **mini-pages** (64 B–4 KB), held in a **circular buffer** (FASTER-hybrid-log
style) that doubles as the write buffer. A mini-page sits "in front of" each leaf page. Reads
hit the mini-page (record cache); writes append to the mini-page (write buffer) and are flushed
to the leaf page only on eviction/checkpoint. This cuts read I/O (cache records, not pages) and
write amplification (persist a record, not a 4 KB page). v2 ports this onto SQLite with a
**single record-granular physiological WAL** for durability, single-writer, configurable
group-commit. **The performance comes from the buffer pool, not the WAL.**

---

## 1. The paper's design (what we are reproducing)

### 1.1 Mini-page (the core abstraction)
- A **record-level cache + write buffer** in front of each leaf page. Variable size; grows/shrinks
  in size classes. Reference impl classes: `[128,192,256,512,960,1856,2048,4096]`; the prior C
  fork used `64,128,256,512,1024,2048,4096` (doubling). Either is fine — pick one and be
  consistent; the codec must store the actual size.
- Holds **four record types** (paper Table 1):

  | Type      | Dirty? | Exists? | Meaning |
  |-----------|--------|---------|---------|
  | Insert    | yes    | yes     | buffered write not yet on the leaf page |
  | Cache     | no     | yes     | clean copy of a record read from the leaf page |
  | Tombstone | yes    | no      | buffered delete (write-back delete) |
  | Phantom   | no     | no      | cached negative-search result (key absent) |

  *Dirty* ⇒ must be written back to the leaf on merge. *Exists* ⇒ answers point lookups without
  touching the leaf. This table is the semantic heart — get it right and most logic follows.
- Records sorted by key; binary search with a 2-byte key **preview** for a fast reject path.

### 1.2 Buffer pool = circular buffer (FASTER hybrid log)
- One large contiguous allocation. **Logical monotonic addresses** (not physical offsets) for
  head/tail to dodge the ABA problem. Allocation bumps the tail; eviction bumps the head.
- A per-allocation 8-byte header (`size`, `state`). State lifecycle:
  `NOT_READY → READY → BEGIN_TOMBSTONE → TOMBSTONE → EVICTED` (+ a freelist state). In the prior
  fork these were lock-free atomics; **for single-writer they are plain loads/stores** (this was
  already done — Stage 3.1, commit `4b68286`).
- Size-classed **free list** for recycling freed chunks.

### 1.3 Eviction + copy-on-access (paper §5.2)
- **Second-chance region** near the head: when an op touches a mini-page there, it is
  **copied to the tail** (copy-on-access), so hot mini-pages never reach the head. Old location
  becomes a tombstone; no eviction triggered.
- **Reference bit** per record: set on access. When copying-on-access, **keep only records with
  the bit set, evict the cold ones**, clear all bits. Cold *clean* records (cache/phantom) are
  discarded free; cold *dirty* records (insert/tombstone) force a **merge of the whole mini-page
  to the leaf** (maximize disk-write utility) while hot records stay.
- Growing a mini-page beyond 4 KB triggers eviction + a leaf **page split**; remaining records are
  routed to the correct side by the split key.

### 1.4 Range scan (§5.3) and negative search (§5.6)
- Range scan must load the leaf page anyway, then **merge** leaf records with mini-page records
  (honoring tombstones). A frequently-scanned mini-page may **grow to full page size** so the
  buffer pool also acts as a page cache.
- Negative search caches a **phantom** record so repeated lookups of an absent key skip the leaf.

### 1.5 Durability — §5.7 (READ THIS TWICE)
- **Orthogonal to the core design; DISABLED for all baselines.** The headline numbers (6× write
  vs B-Tree, 2× point, 2.5× scan vs RocksDB) run with **direct I/O, fsync and WAL OFF**, io_uring
  kernel-polling, OS page cache bypassed, 200 M × 32 B records, Zipf 0.9, 2 GB cache (larger-than-
  memory). **The wins are the buffer pool, full stop.**
- **Logging:** a *simple ARIES-style **physiological** redo WAL*. *"Before any write is committed
  it appends a log entry to the WAL and waits (log full or default 1 ms interval) until flushed.
  The log entry points to a page on disk and describes an operation (insert, delete)."* ⇒
  **group-commit, record-granular, page-addressed.**
- **Checkpoint:** Aurora-style — *async, continuous, offline, by replaying WAL entries in
  parallel*; online snapshot pauses writes and writes back dirty mini-pages + inner pages + a
  virtual→physical mapping table to a snapshot file.
- **Recovery:** (1) rebuild in-memory tree from the snapshot file; (2) replay the WAL — *"find the
  corresponding page and re-apply the operation."*
- **Implication for v2:** the file-format change earns the **write** win (commit persists a small
  record, not a 4 KB page) **and only with relaxed/group-commit durability**. The **read/scan**
  wins need *no* format change — only the buffer pool + a real direct-I/O larger-than-RAM
  benchmark.

### 1.6 Concurrency (NOT in v2 scope — documented for completeness)
Optimistic seqlocks on inner nodes; an upgradeable RwLock on page-table entries; CAS state
machines with RAII guards. Validated with `shuttle` (deterministic interleavings). **v2 keeps
SQLite's single-writer `BtShared` serialization and drops all of this** (a permanent, documented
non-transfer).

---

## 2. How it maps onto SQLite (v2 architecture in brief)

See `BF_TREE_V2_PLAN.md` for the full target. Essentials:

- **Single physiological WAL.** Extend SQLite's WAL with a **record-batch frame kind** (fixed
  page-size frame whose payload packs `[pgno u32][op u8][keyLen varint][valLen varint][key][val]…`)
  alongside normal page-image frames. BF rowid-table leaf mutations → record frames; schema,
  secondary indexes, overflow, freelist, splits → page-image frames. **One log, one recovery
  walk, one commit marker per txn** (cross-log atomicity avoided by construction).
- **Write path:** insert/delete → mini-page (RAM) → append record to WAL. Leaf pages written
  **lazily** at eviction + checkpoint, never at commit.
- **Read path:** reconstruct page P = latest page-image of P + in-order replay of later record
  ops targeting P (from an in-memory `pgno → [(frame,offset)]` index), absorbed by the mini-page
  cache in steady state.
- **Checkpoint** applies record frames by replay-to-page; **recovery** replays to the last commit
  marker; **rollback/savepoint** truncate the WAL tail + drop matching dirty records (no
  flush-at-savepoint-open dance — records were never on the leaf).
- **Durability configurable** via `PRAGMA synchronous`: group-commit (~1 ms) vs strict per-commit.
- **v1 scope:** rowid tables, primary B-tree only. Secondary indexes etc. stay write-through.

---

## 3. Data structures to port verbatim (durability-agnostic)

From the prior fork `src/bf_cache.h` + the three leaf modules. These are debugged and stress-
clean; **port them, don't rewrite them.**

### `BfMiniPage` (24 B header)
`nodeSize u16` · `metaCount u16` · `freeSpace u16` · `prefixLen u16` · `baseDiskOffset i64`
· `ownerPgno u32` (the leaf pgno this mini-page is keyed under) · `rootPgno u32` (owning table
root, for group-flush). Layout: `[24 B header][BfKVMeta[] grows forward][free][key/val data grows
BACKWARD from end]`.

### `BfKVMeta` (8 B per record)
`offset u16` (from page end) · `keyLenAndOp u16` (bits 0-13 keyLen, bits 14-15 op) ·
`valueLenAndRef u16` (bits 0-14 valLen, bit 15 reference/hot) · `preview[2]` (first 2 key bytes).
Op values: `INSERT=0, DELETE=1, CACHE=2, PHANTOM=3`. Dirty = {INSERT, DELETE}.

### `BfAllocMeta` (8 B, precedes each circular-buffer allocation)
`size u32` · `state u8` (lifecycle §1.2) · `reserved[3]`. Found via `(u8*)pData - 8`.

### `BfCircularBuffer`
`pBuffer` · `capacity` (power of 2) · `headAddr`/`tailAddr`/`evictingAddr` (logical) · `freeList`
· `copyOnAccessThreshold` (≈0.9·capacity) · counters. Default 8 MB; copy-on-access ratio 0.1.

### `BfMapEntry` (page → location) + `BfPageLocation`
`locType u8` (NULL/BASE/MINI/FULL) · `pPage void*` · `diskOffset i64`. Batched
`apMap[pgno/256][pgno%256]`, lazy per 256-entry batch, cap `BF_MAP_MAX_BATCHES=65536`.
**Note:** the per-entry rwlock was removed (Stage 3.1) — single-writer; do not re-add it.

### `BfCache` (the per-pager cache; = the pcache2 cache, shared pointer)
Holds `cb` (circular buffer), `apMap`, size classes, dirty tracking, re-entrancy flags
(`bBypassActive`, `bMergingActive`, `bShortcutSuppressed`), `pBtShared` back-pointer, and stat
counters (`nMiniPageHit/Miss`, `nUpgrades`, `nEvictions`, merge/tombstone/writeback counters).

---

## 4. Bugs already mapped — avoid by construction

### 4.1 The 5 mini-page bugs (from the prior fork; `bf_mini_page.c`)
1. **Insert offset frontier** — data grows *backward* from page end; a new record's offset must be
   `MAX(existing offset+len)`, **not** MIN. MIN ⇒ overlapping records.
2. **Compute `maxOffset` BEFORE the metadata shift** — the shift duplicates a slot and hides the
   true frontier, giving a colliding offset on any non-append insert.
3. **`bfBinarySearch` branch direction** — `cmp<0` must do `hi=mid-1` (not `lo=mid+1`). The wrong
   way builds a *descending* array that is self-consistent for point reads (hides for ages) but
   forces idx=0 shifts that trip bug #2.
4. **Canonicalize the rowid delete key** — insert/read encode the rowid big-endian
   (`bfEncodeRowid`, 8 B); delete must use the *same* encoding or tombstones land under an
   unreachable key and deleted rows stay visible.
5. **Tombstone in-place update must bypass the free-space gate** — a tombstone (valLen=0) over an
   existing key on a full 4 KB mini-page must overwrite in place, not be rejected as FULL. Do the
   binary search before the space check; gate only brand-new / growing records.

### 4.2 Integration edge-cases (from `btree.c`/`bf_btree.c` history)
- **Per-leaf keying:** mini-pages are keyed by the cursor's **leaf pgno** (`ownerPgno`), set from
  the descent. `balance_nonroot` redistributing cells between sibling leaves needs a
  forget/flush hook on the old leaves or records become incoherent (was Stage 1.2 bug 72e547d).
- **Eviction is tri-state** (`evictCallback`): orphaned (no map entry) → reclaim; mapped+clean →
  unlink + reclaim; mapped+dirty → in v1 it *refused* (data loss otherwise). **In v2, dirty
  eviction MERGES via the WAL-backed writeback** (no longer refuses) — this is the Phase-3 win.
- **Descent shortcut:** serve point reads from the mini-page **without reading the leaf** (a
  leaf-less cursor flag). Must be suppressed for range seeks and when materializing the cursor
  back onto its physical leaf.
- **Merge-iterating scans:** forward scans interleave buffered inserts with leaf cells in key
  order, suppress tombstoned base cells, and collapse the re-insert-over-tombstone *shadow*
  (new INSERT over a tombstone whose base cell still exists → emit new value, drop base cell).
- **Negative results stay authoritative on the base for write-through index btrees** — a stale
  phantom that overrides a successful base search caused `SQLITE_CORRUPT_INDEX` + swallowed UNIQUE
  errors. In v1, **index BF caching is OFF**; bringing it back needs pre-search short-circuit +
  insert/delete invalidation + one consistent key encoding.
- **Auto-vacuum is unsupported** with BF (pointer-map page moves vs cached records). Disable BF
  when `autoVacuum`.
- **Savepoints (v2 is simpler):** record the WAL frame position at open; `ROLLBACK TO` truncates
  the WAL tail + drops dirty records to that position. No pre-flush invariant (records were never
  applied to the leaf). Full rollback discards the uncommitted WAL tail + dirty records.

---

## 5. Build, run, and the correctness oracle (port from `bench/`)

- **Amalgamation build:** edits live in `src/`; `cd build && make sqlite3` regenerates `tsrc/` →
  `sqlite3.c` → binary. (Trap: `build/tsrc/` and `build/*.c` are generated; never edit them.)
- **Differential oracle (the stop-the-line gate):** `bench/stress.sh` runs identical randomized
  SQL against the BF binary and a **stock** binary (`cc -DSQLITE_OMIT_BF_CACHE … shell.c
  sqlite3.c -o sqlite3_stock`) and diffs query output **and** `.dump` byte-for-byte. Generators:
  `gen_stress.py` (monotonic), `gen_stress_rand.py` (random rowid, page split/merge churn),
  `gen_merge_stress.py` (in-txn scans over buffered inserts), `gen_wb_delete_stress.py`
  (tombstone + shadow). Default matrix: 18 seeds × {delete, wal, memory}.
- **`PRAGMA bf_cache_stats`** exposes hit/miss/upgrade/eviction/merge counters — use them to prove
  a path is actually engaged (e.g. "zero base-page writes at commit").
- The paper's own validation mirror: **differential fuzzing vs a reference model**, **libFuzzer +
  ASAN**, static mini-page safety. v2 adds a **crash-injection VFS** and **ESBMC** on the pure
  modules (WAL codec encode∘decode = identity; mini-page non-overlap; circular-buffer invariants;
  recovery idempotence). See plan §"Testing strategy".

---

## 6. Reference pointers

- Paper PDF: https://vldb.org/pvldb/vol17/p3442-hao.pdf — durability §5.7, eviction §5.2, scan
  §5.3, negative search §5.6, record types Table 1.
- Reference impl: https://github.com/microsoft/bf-tree (`doc/snapshot-recovery.md` for the CPR
  snapshot/recovery details).
- Companion docs: https://github.com/XiangpengHao/bf-tree-docs (mini-page + buffer pool overview).
- Prior C fork (this repo): `src/bf_mini_page.c`, `src/bf_circular_buffer.c`, `src/bf_mapping.c`
  (port these), `src/bf_btree.c` + `src/btree.c` BF hooks (reference for re-implementation),
  `src/bf_pager.c` (the stubbed journal that v2 supersedes), `BF_TREE_DESIGN.md` §7/§13 (the
  "no durability by design" rationale v2 reverses).
- Project memory: `roadmap-bftree-v2` (current driver), `write-buffering-bugfixes`,
  `eviction-flush-gap`, `architecture`, `roadmap-ablation-plan` (superseded history).
