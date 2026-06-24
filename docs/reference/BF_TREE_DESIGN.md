# Bf-Tree in SQLite: Design Document

This document explains the design of the Bf-Tree implementation in this SQLite
fork: what was built, why each decision was made, and — explicitly — where the
implementation diverges from the design described in the Bf-Tree paper and its
reference implementation.

> **Reference design.** "Bf-Tree: A Modern Read-Write-Optimized Concurrent
> Larger-Than-Memory Range Index", Xiangpeng Hao and Badrish Chandramouli,
> PVLDB 17(11), 2024. The reference implementation is a standalone Rust
> storage engine. Throughout this document, "the paper" refers to that design.
> Where this document says "diverges", it means a deliberate or pragmatic
> departure from the paper's architecture, not a bug.

---

## 1. Positioning: a cache layer, not a new index

**The single most important design decision** of this implementation, from
which almost every other difference follows:

* **The paper** builds the Bf-Tree as a *complete index structure*. Inner
  nodes live in memory, leaf pages live on disk, and *mini-pages* sit between
  them as first-class tree nodes: a parent pointer can resolve (through a
  mapping table) either to an on-disk leaf page or to a mini-page in a
  circular in-memory buffer. The mini-page **is** part of the tree.

* **This implementation** keeps SQLite's `btree.c` as the single source of
  truth for index structure, and layers the Bf-Tree machinery on top of it as
  a **record-level read cache + write buffer**. Mini-pages are consulted by
  hooks injected into SQLite's read/write paths; they never participate in
  the B-tree's structural operations (splits, balancing, overflow chains).

**Rationale.** SQLite's B-tree, pager, journal and VDBE are deeply
interlocked (variable-length cells, overflow pages, pointer-map pages for
auto-vacuum, savepoint journaling). Replacing the leaf format outright would
have meant reimplementing crash recovery, file-format compatibility and the
entire test surface. Layering on top preserves:

1. **File format compatibility** — the database file is a normal SQLite file;
   disabling BF (`PRAGMA bf_cache = OFF`) yields stock behavior.
2. **Correctness fallback** — every BF path has a "fall through to the base
   B-tree" escape hatch, so BF can refuse work (cache full, unsupported key
   type) without affecting correctness.
3. **Incremental measurability** — the thesis goal is to evaluate Bf-Tree
   *caching ideas* (record-granularity caching, write buffering, variable
   size pages) against SQLite's page-granularity cache, which this layering
   isolates cleanly.

**Cost.** The implementation cannot realize the paper's full benefit: the
base B-tree pages must eventually absorb every buffered write through
SQLite's normal (page-granularity, journaled) write path, so the write
amplification reduction is *deferral and batching within a transaction*, not
the paper's end-to-end record-granularity persistence.

---

## 2. Component overview

| Component | File | Paper analogue |
|---|---|---|
| Circular buffer (mini-page arena) | `src/bf_circular_buffer.c` | Circular buffer (FASTER-style hybrid log) |
| Mini-page record store | `src/bf_mini_page.c` | Mini-page |
| Mapping table | `src/bf_mapping.c` | Mapping table |
| Cache façade + pcache2 plugin | `src/bf_cache.c` | Buffer manager |
| B-tree hooks | `src/bf_btree.c` + edits in `src/btree.c` | Tree operations (B-tree itself, in the paper) |
| Pager glue | `src/bf_pager.c` + edits in `src/pager.c` | I/O layer |
| Configuration / PRAGMAs | `src/bf_config.c` + edits in `src/pragma.c`, `src/main.c` | Build-time constants |

The central object is `BfCache` (`src/bf_cache.h`). One instance exists per
pager (`Pager.pBfCache`, created lazily in `sqlite3PagerOpenBfCache`,
`src/pager.c:4243`). It owns:

* a **circular buffer** (`BfCircularBuffer`) — default 8 MB, power-of-two
  capacity — from which all mini-pages are allocated;
* a **mapping table** (`BfMapEntry **apMap`) — a direct-indexed, batched
  array keyed by page number;
* statistics and the re-entrancy flags used by the flush machinery
  (`bBypassActive`, `bMergingActive`, `pgnoRootForFlush`).

### 2.1 Dual role of `BfCache` (pcache2 + record cache)

`bf_cache.c` registers a full `sqlite3_pcache_methods2` implementation
(`sqlite3BfCacheSetMethods`, installed at startup from `src/main.c:300`).
That pcache2 implementation is intentionally a **conventional full-page
cache** — hash table, LRU list, page recycling — functionally equivalent to
SQLite's `pcache1`. The mini-page machinery does *not* back the page buffers
returned to the pager; it hangs off the same `BfCache` object and is reached
through the record-level entry points (`sqlite3BfRecordRead/Write`).

When the pcache2 plugin is active, the per-pager cache *reuses* the pcache2
`BfCache` object (cast-compatible: `BfCacheInt.base` is the first member;
see `src/pager.c:4250`) so both layers share one circular buffer; the pager
marks it `bBfCacheShared` so teardown happens exactly once.

**Why this shape.** A pure pcache2 plugin cannot implement Bf-Tree semantics:
the pcache2 interface traffics in opaque full-page buffers keyed by page
number, with no visibility into records or keys. Record-level caching
therefore has to live *above* the pager, in `btree.c`, where keys and
payloads are visible. The pcache2 plugin exists so the whole system runs
under one allocator/configuration umbrella and so a future full-page-mirror
path (`BF_LOC_FULL`, currently unused) has a home.

**Divergence from the paper.** The paper has no separate full-page cache to
reconcile with: inner nodes are pinned in memory and leaf pages are read
around the cache, with mini-pages being *the* caching layer. Here the
relationship is inverted — SQLite's full-page caching remains primary and
mini-pages are a secondary, record-granularity accelerator in front of it.

---

## 3. Mini-pages

### 3.1 Layout (follows the paper)

A mini-page is a variable-length (64–4096 B) slab inside the circular buffer:

```
+--------------------------+
| BfMiniPage header (24 B) |  nodeSize, metaCount, freeSpace, prefixLen,
+--------------------------+  baseDiskOffset, lsn
| BfKVMeta[] (8 B each)    |  grows forward, kept sorted by key
+--------------------------+
| free space               |
+--------------------------+
| key/value heap           |  grows backward from the end
+--------------------------+
```

Each `BfKVMeta` packs, into 8 bytes: a 16-bit offset from the page end, a
14-bit key length + 2-bit **operation type**, a 15-bit value length + 1-bit
**referenced flag**, and a **2-byte key preview** used to short-circuit most
comparisons during binary search. This mirrors the paper's record metadata
design (sorted indirection vector, key prefix in the metadata for cache-line
efficiency, per-record state bits).

The four operation types also follow the paper's record taxonomy:

| Op | Dirty? | Meaning |
|---|---|---|
| `BFOP_INSERT` | yes | buffered write not yet in the base B-tree |
| `BFOP_DELETE` | yes | tombstone |
| `BFOP_CACHE` | no | clean copy of a record that exists in the base page |
| `BFOP_PHANTOM` | no | cached negative lookup ("key does not exist") |

The dirty/clean distinction drives everything downstream: flush only touches
`INSERT`/`DELETE`; eviction refuses pages containing them (§6).

### 3.2 Insert semantics (`sqlite3BfMiniPageInsert`)

Insertion keeps the meta array sorted (memmove on insert — acceptable at
≤4 KB node size; the paper makes the same trade for binary-searchability).
Three deliberate decisions here, all correctness-driven (these were the
subject of the write-buffering bugfix round):

1. **The binary search runs *before* the free-space check.** An update of an
   existing key whose new value is not larger — in particular a tombstone,
   `nVal == 0` — is an in-place overwrite that consumes no space and must
   succeed even on a full mini-page. Gating it on free space would silently
   drop tombstones and resurrect deleted rows.
2. **Grow-in-place updates re-insert.** If the new value is larger, the old
   meta slot is removed and the record re-inserted; the old heap bytes become
   fragmentation reclaimed by the next consolidation. Heap space is never
   compacted inline (no memmove of the data heap on the hot path).
3. **The new heap offset is computed by scanning for the current max offset
   *before* the meta array is shifted** — the shift duplicates a slot, and
   scanning after it would compute a colliding offset (this was an actual
   data-corruption bug class).

### 3.3 Size classes and growth (follows the paper, simplified)

Mini-pages move through power-of-two size classes 64 → 128 → … → 4096. When
an insert fails for lack of space, `sqlite3BfRecordWrite` allocates the next
class from the circular buffer, copies the contents, frees the old slab, and
retries. This matches the paper's growth-by-copy mechanism.

**Divergence:** the upgrade copy takes **all** records, not only referenced
ones. The paper can shed cold records during a copy because everything it
drops still exists on disk; here the mini-page may contain *buffered dirty
inserts that exist nowhere else*, so filtering by the REF bit would lose
committed-in-transaction data. Cold-record shedding happens instead in
`sqlite3BfMiniPageConsolidate`, which is only invoked when the page is clean.

**Divergence:** when a mini-page reaches 4096 B and still cannot fit a
record, the paper merges it into the leaf page and restarts. Here the writer
simply receives `BF_FULL`/`SQLITE_FULL` and **falls through to the normal
base-page write** for that record; the already-buffered records stay dirty
and are applied by the next flush. Forcing a merge at that point would
require re-entering `sqlite3BtreeInsert` from inside itself.

### 3.4 What is *not* implemented from the paper's mini-page

* **Fence keys.** `metaCount` documents "(including fence keys)" and
  `prefixLen` exists in the header, but no fence keys or prefix compression
  are ever created. They are meaningless in this design anyway: a mini-page
  is not bounded by a leaf's key range (§4.1), so it has no fences.
* **LSN / WAL coupling.** The `lsn` field is reserved but never set; see §7.
* **Range gap caching** (a mini-page recording "this whole key range is
  cached") — `BF_LOC_FULL` is declared but no code path creates it.

---

## 4. Keying: one mini-page per *table*, not per leaf page

### 4.1 The decision

**The paper:** mini-pages are per leaf page. Each mini-page buffers the hot
records of one bounded key range, and the inner-node structure routes a key
to exactly one mini-page.

**This implementation:** all BF records for a table are keyed by
**`pCur->pgnoRoot`** — the root page number of the table's B-tree — so there
is exactly **one mapping entry and one mini-page (≤ 4 KB) per table** (see
the header comment of `src/bf_btree.c`).

**Rationale.**

1. **Stability.** A leaf page number is not a stable identity in SQLite:
   splits, balances and auto-vacuum relocate cells between pages constantly.
   Keying by leaf pgno meant buffered records were orphaned the moment the
   tree rebalanced. The root pgno never changes for the life of the table.
2. **Hook placement.** The fast-path existence check in
   `sqlite3BtreeTableMoveto` (`src/btree.c:6053`) runs while the cursor may
   still be at the root; with root keying the lookup works from *any* cursor
   position, including a cursor that BF itself invalidated after buffering an
   insert.
3. **No routing structure needed.** Per-leaf mini-pages would require
   reproducing the paper's parent-pointer indirection inside SQLite's inner
   pages (i.e., modifying the page format) or maintaining a shadow interval
   tree. Per-table keying makes the mapping table a trivial array lookup.

**Cost — this is the largest behavioral divergence from the paper.**

* Buffer capacity per table is capped at one max-size mini-page (4 KB), so
  the write buffer absorbs only small bursts per table before writers fall
  through to base pages.
* There is no per-range locality: a scan-heavy workload over one key range
  and a write-heavy burst over another share one tiny mini-page.
* The paper's "merge mini-page into its leaf" is impossible — records from
  one mini-page scatter across arbitrarily many leaves, which is why flushing
  replays records through a cursor (§5) instead of merging pages.

### 4.2 Key encodings

* **Rowid tables:** the rowid is canonicalized to **8-byte big-endian**
  (`bfEncodeRowid`) on every path — insert, delete, lookup, promote — so that
  `memcmp` ordering matches integer ordering. (An earlier bug stored
  tombstones under native-endian keys that lookups could never match.)
* **Index tables:** index keys are serialized from the `UnpackedRecord` by an
  ad-hoc, lossy encoder (`keyBuf[512]`, type-tagged fields). Index entries
  are used **only** for existence/phantom checks; they never carry payloads
  and never write-buffer. The paper has a single generic byte-string key
  space and none of this asymmetry.

---

## 5. Write buffering and the flush ("merge") path

### 5.1 What gets buffered

The hook inside `sqlite3BtreeInsert` (`src/btree.c:9635`) intercepts an
insert and buffers it as `BFOP_INSERT` **only** when *all* of:

* rowid table (`curIntKey`), root page > 1 (never `sqlite_schema`);
* plain insert: no `BTREE_SAVEPOSITION` (update), no `BTREE_PREFORMAT`
  (cell transfer, e.g. VACUUM), `nZero == 0`;
* payload present and ≤ 4096 bytes.

On success the cursor is deliberately **invalidated** — the row exists only
in the mini-page, so any cursor-relative operation must go through a path
that consults BF or triggers a flush. If buffering fails, the insert falls
through to the normal base-page write, and is then opportunistically cached
clean (`BFOP_CACHE`) so subsequent point reads hit the mini-page
(`src/btree.c:9894`).

**Deletes are write-through, inserts are write-back.** `sqlite3BtreeDelete`
records a tombstone (`src/btree.c:10092`) but *always proceeds with the
base-page delete*. The tombstone is purely a read-cache artifact (it lets
`TableMoveto` answer "not found" without descending). Rationale: a buffered
delete-of-a-base-row would make every scan path wrong unless scans
merged tombstones on the fly, which the per-table keying cannot support.
The paper buffers both directions symmetrically.

### 5.2 Flush by logical replay, not page merge

**The paper** merges a mini-page into its single leaf page: read leaf, apply
deltas, write leaf, done — a physical, page-local operation performed during
eviction.

**This implementation** flushes by **replaying records through SQLite's own
public B-tree API** (`bfFlushOneMiniPage`, `src/bf_btree.c:159`): open a
temporary `BTREE_WRCSR` cursor on the table root, iterate the mini-page, and
call `sqlite3BtreeInsert` / `sqlite3BtreeTableMoveto` + `sqlite3BtreeDelete`
per dirty record; then mark the mini-page clean (INSERT→CACHE,
DELETE→PHANTOM), so the data remains hot in cache after the flush.

**Rationale.** Logical replay is the only flush that is correct under
per-table keying (records span many leaves), and it gets page splitting,
overflow handling, pointer-map maintenance and journaling *for free* by
construction. The cost is that flushing re-enters the very code BF hooks —
handled by two guards:

* `bBypassActive` — set during replay; makes every BF hook return "not
  present" so the replayed inserts are not re-buffered into the mini-page
  they are being drained from;
* `bMergingActive` — prevents `PrepareForScan` from recursing when the
  replay itself navigates the tree.

### 5.3 When flushes happen

| Trigger | Site | Why |
|---|---|---|
| **Before any range scan** of a table with dirty records | `sqlite3BfBtreePrepareForScan`, called from `sqlite3BtreeFirst` / `Next` / `Last` / `Count` (`src/btree.c:5776`, `5878`, `6520`, `10728`) | Scans walk base pages directly; buffered inserts would be invisible. Called both *before and after* `moveToRoot` because the base tree may be completely empty while the mini-page holds every row. Returns 1 so the caller re-roots the cursor. |
| **Commit phase one** | `sqlite3BtreeCommitPhaseOne` (`src/btree.c:4343`), before the pager commits | The only persistence step for buffered rows; must precede journaling/sync. |
| **Savepoint open** | `btreeOpenSavepoint` (`src/btree.c:3613`) | Flushing at savepoint creation means any record still dirty at savepoint-rollback time was buffered *after* the savepoint opened, making "discard the whole BF cache" a correct rollback action. |
| **Rollback** | `sqlite3BfBtreeClearCache` (`src/btree.c:4561`, `4680`) | Not a flush — the inverse: unlink every mapping entry so rolled-back buffered inserts vanish. The slabs are not freed; FIFO eviction reclaims them as orphans. |

**Divergence:** the paper flushes (merges) on *eviction pressure*; this
implementation flushes on *visibility and durability boundaries* (scan,
commit, savepoint) and refuses eviction of dirty pages instead (§6). The
paper has no analogue of the scan-prep flush because its scans read
mini-pages natively as part of the tree; here the base tree must be made
whole before SQLite's native scan code runs.

### 5.4 Partial-failure policy

If some records fail to apply during a flush, the mini-page is *not* marked
clean — failed records stay dirty and are retried at the next flush point
(`ctx.nErrors` check in `bfFlushOneMiniPage`). Flush is idempotent for
inserts (replaying an applied insert overwrites the same rowid with the same
payload), which is what makes retry safe.

---

## 6. Circular buffer: allocation, reuse, eviction

### 6.1 What follows the paper

`bf_circular_buffer.c` is a faithful, scaled-down rendition of the paper's
FASTER-inspired arena:

* **Monotonic logical addresses** (`headAddr`/`tailAddr`, physical position =
  `addr & (capacity-1)`); allocation bumps the tail; an 8-byte `BfAllocMeta`
  precedes every slab.
* **Allocation lifecycle state machine** — `NOT_READY → READY →
  BEGIN_TOMBSTONE → TOMBSTONE → EVICTED`, plus `FREELISTED` — driven by
  atomic CAS (`bfTryBeginTombstone`) with acquire/release ordering, so an
  eviction sweep and a writer can race safely on the state byte.
* **Per-size-class free lists** threading next-pointers through the freed
  slabs themselves, so deallocated mini-pages are recycled without consuming
  the tail.
* **Wrap handling** by stuffing a tombstone into the unusable space at the
  physical end of the buffer.
* **A copy-on-access region** near the head, computed as
  `distance_to_tail ≥ capacity × (1 − 0.1)`: slabs deallocated inside that
  region are tombstoned rather than free-listed, because the head will reach
  them soon and free-list reuse there would fight the sweep.

### 6.2 Divergences

1. **Copy-on-access is only half of the paper's mechanism.** The paper's
   defining trick is that *reading* a mini-page inside the eviction region
   copies it to the tail, so hot data perpetually escapes the advancing head.
   Here `sqlite3BfCircularBufferIsCopyOnAccess` is consulted only on the
   *deallocation/reuse* paths; **no read ever relocates a mini-page**. Hot
   clean data therefore does get evicted under pressure (and is re-promoted
   later); hot *dirty* data is protected by the refusal mechanism below
   instead. This was a deliberate simplification: relocating a mini-page
   means atomically updating the mapping entry mid-read, which needs the
   paper's optimistic-locking protocol (§8) that this single-threaded
   integration does not carry.

2. **Eviction is synchronous and refusal-based; the paper's is a merging
   sweep.** There is no background eviction thread. When allocation fails,
   the caller runs a bounded FIFO sweep (`sqlite3BfCacheEvict`, 10–16
   entries) inline. For each head slab the callback (`evictCallback`,
   `src/bf_cache.c:943`) classifies:

   * **Orphaned** (no mapping entry points at it — rollback leftovers,
     superseded size-class slabs): reclaim freely.
   * **Mapped + clean**: unlink the mapping entry, reclaim.
   * **Mapped + dirty**: **refuse**. The sweep aborts entirely (FIFO order
     means nothing behind the head can advance), the allocation fails, and
     the writer falls back to the base-page path.

   The paper instead *merges* a dirty mini-page to disk as part of eviction.
   Here that is impossible from arbitrary call depth: applying records needs
   a `Btree` handle and a re-entrancy-safe call point, and the sweep can be
   triggered from deep inside `sqlite3BtreeInsert`. Refusing is the
   never-lose-data choice; the dirty page drains at the next flush boundary
   (§5.3). The accepted consequence is that **one dirty mini-page at the
   head can stall all reclamation** until the next scan/commit flush.

3. **Ownership is resolved by scanning the mapping table.** A mini-page does
   not store its owning pgno (the paper's mini-pages are reached *from* the
   mapping table, never the reverse). The eviction callback finds the owner
   by linear scan (`bfEvictFindEntry`). Acceptable because the map holds one
   entry per *table* with BF state (a handful), not one per page.

4. **`FREELISTED` slabs encountered by the sweep are force-evicted**, leaving
   a benign dangling free-list pointer (documented at
   `src/bf_circular_buffer.c:580`): the allocator re-validates anything it
   pops from the free list, and a stale hit just degrades into a fresh tail
   allocation. The paper's allocator/sweep coordination is stricter; this
   shortcut is acceptable under SQLite's effectively-serialized access.

---

## 7. Durability: none (deliberately)

**The paper:** mini-pages are durable. Record writes are logged to a WAL
keyed by LSN (`BfMiniPage.lsn` is the vestige of this here); a crash replays
the log and reconstructs mini-page state; a buffered insert never needs to
reach the leaf page to survive.

**This implementation: the mini-page layer has zero durability.** Buffered
inserts live only in memory; the commit-phase-one flush pushes them through
SQLite's normal pager → journal → sync pipeline *before* the transaction
commits, so the *transaction* durability contract is preserved exactly —
but by paying the full base-page write cost at commit. The journaling
scaffolding in `bf_pager.c` (`BF_JOURNAL_MAGIC`, `JournalMiniPage`,
`ReplayMiniPage`) is stubbed and unreachable.

**Rationale.** Making mini-pages durable inside SQLite means inventing a
second log alongside the rollback journal/WAL and teaching recovery to
replay it — a file-format change with an enormous correctness surface
(P0.2 in `BF_TREE_IMPLEMENTATION_PLAN.md` gated this off explicitly). The
chosen semantics — "BF is a transparent cache; commit makes everything real
through the stock path" — keeps crash recovery byte-identical to stock
SQLite. The measurable benefit narrows to: point-read acceleration, write
deferral/batching within a transaction, and negative-lookup caching.

---

## 8. Concurrency model

**The paper** is aggressively concurrent: lock-free reads against the
circular buffer, optimistic lock coupling on the tree, per-mapping-entry
read-write locks, background eviction.

**This implementation** inherits SQLite's concurrency model instead: all
B-tree access for a database is serialized by the `BtShared` mutex, so the
BF layer is effectively single-threaded per database. Concretely:

* The circular buffer takes a plain mutex around alloc/dealloc bookkeeping,
  and the allocation state machine keeps its atomics (cheap insurance, and
  they document the intended lifecycle), but nothing exercises them
  concurrently in practice.
* Per-entry reader-writer spinlocks exist in the mapping table
  (`sqlite3BfMapLockRead/Write`, CAS-based with sleep-backoff) but are **not
  taken on the hot paths** — lookups and mutations rely on the `BtShared`
  serialization. They are forward-provisioning for a multi-threaded pcache
  scenario.
* The mapping table itself is a **direct-indexed array** (batches of 256
  entries, up to 4096 batches ⇒ pgno < 1,048,576) rather than the paper's
  hash-style mapping of mini-page IDs. O(1) lookup, zero probing, and the
  per-table keying means almost all batches are never allocated. The hard
  pgno cap is an accepted limitation (4 GB database at 4 KB pages).

---

## 9. Read path and promotion policy

Point-read flow for a rowid table:

1. **`sqlite3BtreeTableMoveto`** consults BF *after* the normal descent
   (`src/btree.c:6053`): a hit short-circuits the result (`found` /
   `not-found-by-tombstone`), and a confirmed base-tree miss is cached as a
   **phantom** so the next probe for the same absent key skips the descent.
2. **`sqlite3BtreePayload`** tries `sqlite3BfBtreeFetchPayload` first
   (`src/btree.c:5391`); a mini-page hit serves the payload without touching
   base pages (with a malloc'd staging buffer when `offset > 0`, since the
   mini-page stores whole values).
3. On a BF miss, after the base page satisfies the read, the record is
   **promoted** into the mini-page as `BFOP_CACHE` — but only
   probabilistically: `sqlite3_randomness() % 100 < promotionRate`, default
   **5%**, settable via `PRAGMA bf_promotion_rate`.

**Why probabilistic promotion.** The paper promotes via its copy-on-access
machinery and can afford to cache aggressively because eviction is cheap and
hot data self-preserves. Here, with one 4 KB mini-page per table and no
read-driven relocation, promoting every read would thrash the mini-page
(every promotion is an insert that can trigger size-class copies and
evictions). Random sampling is a classical cheap filter: a record read N
times has a `1-(0.95)^N` chance of residing in cache, so genuinely hot
records converge to cached while one-off scans rarely pollute. Promotion is
also restricted to *full-payload* reads of rowid tables — index records are
never promoted because index lookups have no BF read path, so they would be
unreachable dead weight (comment at `src/btree.c:5400`).

**Phantom caching** (negative lookups, including the index variant
`sqlite3BfBtreeCachePhantomIndex`) is comparatively aggressive — always on —
because phantoms are tiny (key + 0-byte value) and uniquely valuable: they
are the only way to skip a full descent for absent keys, the dominant cost
of uniqueness checks on inserts.

---

## 10. Transaction-correctness decisions (no paper analogue)

These exist purely because BF lives inside a transactional SQL engine; the
paper's KV-store setting has none of them:

* **Rollback ⇒ drop everything.** `sqlite3BfBtreeClearCache` unlinks every
  mapping entry. Clean entries could in principle survive, but rollback also
  reverts base pages, so a `BFOP_CACHE` record may describe a row version
  that no longer exists. Discarding all is the only cheap-and-correct option.
* **Savepoint open ⇒ flush first** (§5.3) so savepoint rollback can reuse
  the same drop-everything operation safely.
* **Schema is never buffered** (`pgnoRoot > 1` checks, plus `pgno <= 1`
  guard in the flush iterator): schema cooking depends on base pages, and a
  buffered `sqlite_schema` row would deadlock the flush (which itself needs
  the schema).
* **Cursor invalidation after a buffered insert** (§5.1) forces every
  subsequent cursor operation through a re-seek, which is what makes the
  TableMoveto BF check reachable at the right time.

---

## 11. Configuration surface

| Knob | Default | Mechanism |
|---|---|---|
| Enable/disable | **on** | `PRAGMA bf_cache`, `SQLITE_CONFIG_BFCACHE`, compile-time `SQLITE_OMIT_BF_CACHE` |
| Circular buffer size | 8 MB (`BF_DEFAULT_BUFFER_SIZE`; rounded up to power of two) | `PRAGMA bf_cache_size`, `SQLITE_CONFIG_BFCACHE_SIZE` |
| Promotion rate | 5% | `PRAGMA bf_promotion_rate` |
| Copy-on-access region | 10% of capacity | compile-time only |
| Stats | — | `PRAGMA bf_cache_stats` (hits, misses, upgrades, merges, evictions) |

Note: two comments in `bf_config.c` state stale values ("32 MB", "1%");
the authoritative constants are in `bf_cache.h` (8 MB, 5%).

Debug builds (`SQLITE_BF_DEBUG`) write allocation/map traces to
`build/bf-allocs.log` and `build/bfmap-writes.log`.

---

## 12. Summary of divergences from the reference design

| # | Aspect | Paper / reference implementation | This implementation | Driver |
|---|---|---|---|---|
| 1 | Role | Bf-Tree *is* the index; mini-pages are tree nodes | Record cache + write buffer layered over SQLite's unmodified B-tree | File-format compatibility, recovery reuse, fallback safety |
| 2 | Mini-page scope | One per leaf page / bounded key range, with fence keys | **One per table**, keyed by root pgno; no fence keys, no prefix compression | No stable leaf identity in SQLite; no routing structure needed |
| 3 | Flush/merge | Physical merge of mini-page into its leaf page, during eviction | **Logical replay** through `sqlite3BtreeInsert/Delete` via a bypass cursor, at scan/commit/savepoint boundaries | Records span many leaves under per-table keying; reuses split/overflow/journal logic |
| 4 | Durability | Mini-pages WAL-logged (LSN), survive crashes without merging | **None** — memory only; commit-time flush through the stock pager path; `lsn` unused, journal hooks stubbed | Avoid a second recovery log; keep crash behavior identical to stock SQLite |
| 5 | Eviction of dirty data | Sweep merges dirty mini-pages to disk | Sweep **refuses** dirty mini-pages and aborts; writer falls back to base-page writes | Cannot re-enter the B-tree from arbitrary call depth; never lose buffered data |
| 6 | Copy-on-access | Reads in the eviction region copy data to the tail (hot-data preservation) | Region only affects dealloc/free-list policy; **reads never relocate** | Relocation requires the paper's optimistic mapping-entry protocol |
| 7 | Concurrency | Lock-free reads, optimistic locking, background eviction | Mutex-protected, serialized by `BtShared`; rwlocks present but unused on hot paths; eviction inline | SQLite's threading model already serializes access |
| 8 | Mapping table | Maps mini-page IDs, supports relocation | Direct-indexed array keyed by pgno (cap ~1M pages), one live entry per table | O(1), trivial, sufficient under per-table keying |
| 9 | Read caching | Promote-on-access via the buffer machinery | **Probabilistic promotion** (default 5%), full-payload rowid reads only | One small mini-page per table would thrash under promote-always |
| 10 | Deletes | Write-buffered symmetrically with inserts | **Write-through** (tombstone is read-cache only); inserts are the only write-back path | Scans can't merge tombstones under per-table keying |
| 11 | Key types | Uniform byte-string keys | Rowid tables: full support (8-byte BE encoding). Index trees: existence/phantom only, lossy 512-B serialization | Index lookups have no BF read path; payload semantics differ |
| 12 | Size-class upgrade copy | May shed cold (unreferenced) records | Copies **all** records | Dirty records exist nowhere else; shedding them loses data |
| 13 | Transactions | Not applicable (KV engine) | Rollback drops cache; savepoints force pre-flush; schema exempt; cursor invalidation after buffering | SQL transaction semantics |
| 14 | Auto-vacuum | Not applicable | **Unsupported**: BF is disabled outright on auto-vacuum DBs (`btreeUsesBfCache` returns 0 when `pBt->autoVacuum`). Auto/incremental vacuum moves pages via `relocatePage`, which would orphan the pgno-keyed cache; rather than re-key on every move we forbid the combination (debug assert in `relocatePage`). The DB still works, just without BF. | Page relocation breaks pgno-keyed mini-pages (Stage 1.5) |

## 13. Known gaps / future work

* Implement true copy-on-access on the read path (needs mapping-entry
  relocation protocol) — would restore the paper's hot-data preservation.
* Background or flush-capable eviction so a dirty head slab cannot stall the
  sweep between flush boundaries.
* Use `BF_LOC_FULL` for range-gap caching of fully-mirrored pages.
* Mini-page durability via WAL coupling (paper's LSN design; currently the
  whole `bf_pager.c` journal path is stubbed).
* Per-range (not per-table) mini-pages, the prerequisite for most of the
  above — requires a stable range identity layered over SQLite's leaves.
* Lift the ~1M-pgno mapping-table cap; remove vestigial fields
  (`prefixLen`, `lsn`, `xStress`) or implement them.
