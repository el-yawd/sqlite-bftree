# Bf-Tree Ablation Implementation Plan

This plan converts the current per-table Bf-Tree cache (see `BF_TREE_DESIGN.md`)
into a per-leaf design faithful to the Bf-Tree paper, in four measured phases.
It is written to be executed phase by phase, stage by stage, by an implementer
who has NOT followed the project history. Read §1 completely before writing
any code. Do not start a phase before the previous phase's acceptance
checklist passes.

**Goal recap.** Each phase closes one divergence from the Bf-Tree paper
(PVLDB 17(11), 2024) and is benchmarked against the previous phase, producing
an ablation curve for the thesis. Two divergences are permanently out of
scope and must NOT be attempted: mini-page durability (would require a second
recovery log; the DB file must remain a stock-compatible SQLite file) and
lock-free concurrency (SQLite serializes all B-tree access via the `BtShared`
mutex; concurrent machinery is unmeasurable here).

---

## 1. Ground rules — read before any code

### 1.1 Repository layout and the build trap

- **Edit ONLY files under `src/`.** The build tree at `build/` contains
  *copies* of the sources under `build/tsrc/`. Editing `build/tsrc/*` loses
  your work on the next build and has caused "my fix didn't take effect"
  confusion before.
- Build: `cd build && make -j$(nproc) sqlite3`. After building, **verify your
  edit is present in the binary's source copy**:
  `grep "<some string you added>" build/tsrc/<file>.c` — if absent, the
  tsrc copy is stale; run `make clean` in `build/` and rebuild. Never "fix"
  staleness by editing `build/tsrc`.
- The benchmark harness lives in `bench/` (`bf_bench.c`, `run.sh`); it links
  two binaries, `bf_bench` (this fork) and `bf_bench_stock` (stock SQLite).

### 1.2 Architecture you are modifying (5-minute version)

Full background: `BF_TREE_DESIGN.md`. The minimum you need:

- `BfCache` (`src/bf_cache.h`) owns a **circular buffer** (FIFO arena,
  default 8 MB) from which variable-size (64–4096 B) **mini-pages** are
  allocated, and a **mapping table** `apMap` — a direct-indexed array keyed
  by page number (`sqlite3BfMapLookup` / `sqlite3BfMapGetOrCreate` /
  `sqlite3BfMapRemove`, `src/bf_mapping.c`).
- A mini-page (`src/bf_mini_page.c`) stores records as a sorted
  `BfKVMeta[]` + a heap. Each record has an **operation type**:
  `BFOP_INSERT` / `BFOP_DELETE` are **dirty** (data exists nowhere else!);
  `BFOP_CACHE` / `BFOP_PHANTOM` are clean (safe to drop at any time).
- Hooks in `src/btree.c` call into `src/bf_btree.c`. Today everything is
  keyed by `pCur->pgnoRoot` (one mini-page per **table**). This plan re-keys
  to the **leaf page number** (one mini-page per hot **leaf**).
- Dirty records are persisted by **logical replay**: `bfFlushOneMiniPage`
  (`src/bf_btree.c:159`) opens a temp `BTREE_WRCSR` cursor and re-executes
  each dirty record through `sqlite3BtreeInsert`/`Delete`, with
  `pBf->bBypassActive=1` so the replayed operations don't re-enter BF, and
  `pBf->bMergingActive=1` so scan-prep doesn't recurse. Flushes happen at:
  scan start (`sqlite3BfBtreePrepareForScan`), commit
  (`src/btree.c:4343`), savepoint open (`src/btree.c:3613`). ROLLBACK calls
  `sqlite3BfBtreeClearCache` (drop everything, `src/btree.c:4561,4680`).
- Eviction (`sqlite3BfCacheEvict`, `src/bf_cache.c`) is a synchronous FIFO
  sweep from the buffer head; the callback (`evictCallback`,
  `src/bf_cache.c:951`) reclaims orphaned and clean slabs but **refuses
  dirty ones** (aborts the sweep — never lose dirty data).

### 1.3 Invariants that must survive every change

These have each been the subject of a past correctness bug. Violating any of
them corrupts user data silently.

1. **Rowid keys are always 8-byte big-endian** (`bfEncodeRowid`,
   `src/bf_btree.c:81`) so `memcmp` order == numeric order. Every new code
   path that touches a rowid key must use this encoder. Never store a
   native-endian rowid.
2. **A dirty record (`BFOP_INSERT`/`BFOP_DELETE`) must never be dropped**
   except by ROLLBACK's deliberate ClearCache. Eviction, size-class copies,
   rebalance handling, scan handling — all must either keep dirty records
   reachable from the mapping table or apply them to the base tree first.
3. **Tombstone updates must succeed on a full mini-page.** In
   `sqlite3BfMiniPageInsert`, the binary search for an existing key runs
   BEFORE the free-space check; in-place same-size/smaller overwrites must
   not be gated on free space. Do not "simplify" this ordering.
4. **Never re-enter BF from BF.** Any code that calls back into
   `sqlite3BtreeInsert`/`Delete`/`Moveto` must set `bBypassActive` (and
   `bMergingActive` when navigating) around the call, and must restore them
   on ALL exit paths including errors (use a single cleanup point).
5. **`sqlite_schema` (root page 1) is never buffered or cached.** Keep every
   `pgnoRoot > 1` / `pgno <= 1` guard you encounter.
6. **Fall-through is the universal escape hatch.** Every BF operation must
   have a path that says "BF refuses; do the stock thing". When in doubt
   about a corner case, make BF refuse (return `BF_NOT_FOUND`/`BF_FULL`) —
   slower is acceptable, wrong is not.

### 1.4 Testing protocol (applies to every stage of every phase)

1. **Differential stress test** — the project's main correctness oracle:
   run identical randomized SQL workloads against this fork and stock
   `sqlite3`, then compare both the query outputs and the final database
   image (`.dump` output) byte-for-byte. A script exists from earlier rounds
   (see `bench/run.sh` and the `*.sanity` files for the pattern); if you
   cannot find a ready script, write `bench/stress.sh` in that style first.
   Workloads must include: random INSERT/UPDATE/DELETE/SELECT mixes, UNIQUE
   indexes, multi-statement transactions, SAVEPOINT + ROLLBACK TO, WAL and
   rollback-journal modes, and `PRAGMA integrity_check` at the end. Run ≥ 18
   seeds. **Any divergence from stock is a stop-the-line bug.**
2. **SQLite's own test suite (smoke level)**: `cd build && make testfixture
   && ./testfixture ../test/veryquick.test`. Pre-existing failures (if any)
   must be recorded BEFORE your change and not grow.
3. **Bench sanity**: `bench/run.sh` must complete and `PRAGMA
   bf_cache_stats` must show plausible numbers (nonzero hits on read
   workloads).
4. Build with asserts during development: `OPTS="-DSQLITE_DEBUG=1
   -DSQLITE_BF_DEBUG=1"` — and do a release build (no DEBUG) before
   benchmarking. Never quote benchmark numbers from a debug build.

### 1.5 Code quality requirements

- Match SQLite's house style exactly: `/* */` comments only, ≤ 80 columns,
  2-space indents, `u8/u16/u32/u64/i64` typedefs, functions documented with
  a `/* ... */` block above them stating contract + return codes.
- All new BF code compiles away under `SQLITE_OMIT_BF_CACHE` — guard new
  hooks in `btree.c` the same way neighbouring BF hooks are guarded.
- `assert()` every invariant from §1.3 at the point you rely on it.
- No heap allocation on hot paths (lookup/insert hooks). Stack buffers ≤
  512 B are fine; anything bigger needs justification in a comment.
- No `printf`/file I/O outside `#ifdef SQLITE_BF_DEBUG`.
- Commit per stage, message style: `bf-tree: <stage id> <summary>`. Never
  commit with a failing differential stress run.
- When a stage says "fallback", implement the fallback FIRST, verify
  correctness, then attempt the optimization. A correct slow checkpoint you
  can retreat to is part of the deliverable.

---

## Phase 0 — Measurement rig + correctness debt

Everything later is judged by this phase's harness. Do it first, do it well.

### Stage 0.1 — Remove the broken index-key encoder

`sqlite3BfBtreeRecordExistsIndex` (`src/bf_btree.c:510`) and
`sqlite3BfBtreeCachePhantomIndex` (`src/bf_btree.c:628`) serialize an
`UnpackedRecord` into a byte string with **no type tags** (an INTEGER and a
REAL can collide), **silently skipped fields** when the 512-byte buffer
overflows (two long keys collide), and ambiguous NULL/empty-string
encodings. A collision returns a wrong exists/not-exists answer — a
correctness bug, not a perf issue.

BF record caching for index btrees was already disabled at the call-site
level in an earlier bugfix round (the `IndexMoveto` override was removed).
Therefore: **delete both functions, their declarations in `bf_cache.h`, and
any remaining callers** (verify with `grep -rn "ExistsIndex\|PhantomIndex"
src/`). Add a comment in `bf_btree.c` noting that index caching, if ever
re-added, requires an *injective* encoder (type tag byte per field, hard
failure instead of field skipping, collation-aware) — do not re-add it in
this plan.

Edge cases: make sure deletion doesn't orphan `#include` or stats counters;
build both with and without `SQLITE_OMIT_BF_CACHE`.

### Stage 0.2 — Three-way benchmark control

Today's A/B (`bf_bench` vs `bf_bench_stock`) confounds two variables: the
mini-page machinery AND the fact that this fork's pcache2 plugin replaces
SQLite's `pcache1`. Add the missing middle configuration:

- **Config A**: stock SQLite, stock pcache1 (existing `bf_bench_stock`).
- **Config B**: this fork, BF record layer disabled (`PRAGMA bf_cache=OFF`
  or `SQLITE_CONFIG_BFCACHE` 0) but the pcache2 plugin still active.
- **Config C**: this fork, everything on.

Modify `bench/bf_bench.c` / `bench/run.sh` so one invocation runs all three
configs over each workload and emits a CSV row per (config, workload,
metric). **Validation gate: B must be within noise (±5%) of A on every
workload.** If it is not, the page-cache reimplementation is a confound —
profile and fix it (or document the gap) before quoting any C-vs-A number.

### Stage 0.3 — Workload matrix

Implement as named workloads in `bf_bench.c` (deterministic seeds, parameters
on the command line):

| Name | Shape | Exposes |
|---|---|---|
| `point-zipf` | N-row table, Zipfian point SELECTs by rowid (θ≈0.99) | read caching |
| `point-uniform` | uniform random point SELECTs | promotion-policy cost |
| `point-miss` | SELECTs for absent rowids | phantom caching |
| `insert-auto` | random-key INSERTs, autocommit (1/txn) | buffering overhead floor |
| `insert-batch` | random-key INSERTs, 1000/txn | sorted-replay benefit |
| `mixed` | 80% point reads / 15% writes / 5% short range scans, interleaved in one connection | scan-flush pathology |
| `unique-ins` | INSERTs into table with a UNIQUE column | uniqueness-check descents |
| `ltm-read` | DB ≫ cache (e.g. 2 GB data, `PRAGMA cache_size` ~32 MB, BF buffer 8–32 MB), `point-zipf` shape | larger-than-memory — the headline experiment |

Metrics per run: wall time, ops/sec, and from `PRAGMA bf_cache_stats`:
mini-page hit rate (hits/(hits+misses)), upgrades, evictions. For `ltm-read`
also capture OS-level read I/O if easy (`/proc/self/io` rchar/read_bytes).
**Hit rate is the primary intermediate metric for the thesis** — always
record it.

Pitfalls: drop OS page cache effects by either using a ramdisk consistently
(measures CPU path) or `sync && echo 3 > /proc/sys/vm/drop_caches` between
runs (measures I/O path) — pick one per experiment and label it; run ≥ 5
repetitions and report median; build benchmarks `-O2`, no `SQLITE_DEBUG`.

### Stage 0.4 — Baseline snapshot

Run the full matrix on the current per-table implementation. Commit the CSVs
under `bench/results/phase0-baseline/`. This is ablation point #1; the
current "~30% faster point lookups" claim gets reproduced or corrected here.

### Phase 0 acceptance
- [ ] Index encoder gone; builds with and without `SQLITE_OMIT_BF_CACHE`.
- [ ] Differential stress ≥ 18 seeds clean.
- [ ] Three-way harness runs; Config B ≈ Config A documented.
- [ ] Baseline CSVs committed.

---

## Phase 1 — Per-leaf mini-pages — THE KEYSTONE

Re-key every mini-page from the table's root pgno to the **leaf page the
record lives on**. The core insight: SQLite's own descent through inner pages
*is* the paper's routing structure — the parent page yields the leaf pgno
before the leaf is loaded, so the existing pgno-keyed mapping table works
unchanged and **no file-format or page-format change is needed**.

What this buys: buffer capacity proportional to the hot set (vs 4 KB/table),
range locality, and it unblocks Phases 2–3.

### Stage 1.1 — Plumbing: ownership and metadata

1. Add the owning pgno to the slab: extend the mini-page header or (better)
   store it next to `BfAllocMeta` so the eviction sweep can find the owner
   without the current linear map scan (`bfEvictFindEntry`,
   `src/bf_cache.c:920`). With per-leaf keying the map will hold hundreds of
   entries — the linear scan must go. Replace its use in `evictCallback`
   with a direct `sqlite3BfMapLookup(ownerPgno)` + verify
   `pEntry->pPage == slab` (the entry may have been re-pointed; if it
   doesn't match, the slab is orphaned — reclaim it).
2. Also record the table's **root pgno** in the same place. Flush grouping
   ("flush all dirty leaves of table X") and ClearCache need it.
3. The mapping-table cap (`BF_MAP_MAX_BATCHES`, 4096 ⇒ pgno < 1,048,576,
   `src/bf_cache.h:207`) blocks the `ltm-read` workload on big files. Raise
   it (e.g. 65536 batches ⇒ 16M pages = 64 GB at 4 KB) — the batch-pointer
   array is `nBatches * sizeof(void*)`, still small. Keep an explicit bounds
   check that *refuses* (no BF for that pgno) rather than overflowing.

### Stage 1.2 — Re-key the read path (conservative first)

In `sqlite3BtreeTableMoveto`'s BF hook (`src/btree.c:6053`) and the payload
path (`sqlite3BfBtreeFetchPayload` via `src/btree.c:5391`), replace
`pCur->pgnoRoot` keying with the **current leaf**: the cursor's current page
is `pCur->pPage` (`src/btreeInt.h:555`); use `pCur->pPage->pgno` and only
when `pCur->eState==CURSOR_VALID && pCur->pPage->leaf`. The existence check
now runs AFTER the descent completes (it can no longer run "from any cursor
position" — that property was a per-table artifact; delete the stale comment
in `bf_btree.c`'s header block).

Promotion (`sqlite3BfBtreePromoteRecord`) and phantom caching
(`sqlite3BfBtreeCachePhantom`) likewise key by the leaf the cursor is on.
Phantom subtlety: a "not found" descent still lands the cursor on the leaf
where the key *would* live — that leaf is the correct home for the phantom,
and a later split of that leaf must invalidate or re-route it like any other
record (Stage 1.4 handles this automatically).

At this checkpoint a BF hit no longer skips the descent, so `point-zipf`
in-memory numbers will temporarily DROP relative to baseline. Expected;
note it and continue.

### Stage 1.3 — Re-key the write path

The insert hook (`src/btree.c:9640`) currently buffers before any descent.
Under per-leaf keying the target leaf must be known first, so:

1. Move the hook to run AFTER `sqlite3BtreeInsert` has positioned the cursor
   (after the `loc`/moveto logic, immediately before cell construction).
   The buffered insert then goes into the mini-page of `pCur->pPage->pgno`
   and the base-page cell write is skipped, exactly as today.
2. Keep all current gating: rowid tables only, `pgnoRoot>1`, no
   `BTREE_SAVEPOSITION`, no `BTREE_PREFORMAT`, `nZero==0`, payload ≤ 4096.
   Keep the post-buffer cursor invalidation.
3. **New gating**: refuse to buffer when `loc==0` (key already exists in the
   base leaf — an UPDATE-shaped insert). Today's per-table flush replays
   blindly; with per-leaf bookkeeping it stays correct, but the merge logic
   in Phase 2 is much simpler if a mini-page INSERT for a key that also
   exists in the leaf can only mean "shadow/update". You may buffer it, but
   then write the Phase 2 merge rules accordingly; the safer Phase 1 choice
   is to fall through to the base write for updates. Choose fall-through.
4. Delete tombstoning (`src/btree.c:10092`) keys by the leaf the delete
   descended to. Deletes remain write-through in this phase.
5. The fall-through clean-caching write (`src/btree.c:9900`) keys by
   `pCur->pPage->pgno` as well — but note that after the base insert, a
   balance may already have run; only cache when the cursor is still VALID
   on a leaf, otherwise skip.

Edge cases: empty table (root page IS the leaf — pgnoRoot==leaf pgno is fine,
guards already exclude page 1 only); `INTKEY` check unchanged; a buffered
insert must NOT mark the base leaf dirty in the pager (it doesn't touch it).

### Stage 1.4 — Survive rebalance (the hard part)

When `balance_nonroot` (`src/btree.c:8379`) redistributes cells among
leaves, any mini-page keyed by an involved leaf describes a stale key range.
Dirty records are NOT in the leaf cells, so balance doesn't move them — you
must re-route them yourself.

**Implement the fallback first (1.4a):** at the top of `balance_nonroot`
(and `balance_deeper`, `src/btree.c:9183`, and `btreeOverwriteCell` is not
needed), for every page participating (the old pages `apOld[]` — at entry
you know their pgnos), look up the mapping entry; if present:
- clean records only → `sqlite3BfMapRemove` + dealloc the slab (drop).
- any dirty record → move the slab pointer onto a small pending list hung
  off `BfCache` (`pPendingRebalance`, capacity ~8; if full, see below) and
  remove the map entry. Do NOT free the slab.

Then at the next **safe point** — end of `sqlite3BtreeInsert` /
`sqlite3BtreeDelete`, after `balance()` has fully returned and before the
function exits — drain the pending list: for each record in each pending
slab, re-buffer it through the normal per-leaf write path (fresh descent via
a temp cursor with `bBypassActive` semantics adjusted: bypass must be OFF for
the mapping write but the descent must not recurse into scans — reuse the
`bMergingActive` guard). If re-buffering fails (`BF_FULL`), apply the record
directly to the base tree the way `bfApplyOneRecord` does. If the pending
list itself would overflow, flush the oldest pending slab's records to the
base tree immediately at the same safe point. **Records may never be lost
between balance and drain — assert the pending list is empty at commit,
rollback (ClearCache must also free pending slabs), and btree close.**

Re-entrancy trap: balance also runs during flush replay (`bBypassActive`
set). In that case the mini-page being flushed is being iterated while
balance might try to detach it. Rule: when `bBypassActive` is set,
Stage 1.4a only handles mini-pages OTHER than the one currently being
flushed (`pBf->pgnoRootForFlush` tells you the table; compare slab owner
pgno against the leaf being drained — simplest correct rule: the flush
must detach its mini-page from the map BEFORE replaying (take ownership),
so balance never sees it; restate flush accordingly in Stage 1.5).

**Optimization (1.4b, only after 1.4a passes stress):** inside
`balance_nonroot`, after the new pages (`apNew[]`) are finalized, re-route
records in place instead of deferring: decode each record's rowid (8-byte BE
→ i64), read the last-cell rowid of each `apNew[i]` from the in-memory page
(intkey pages: parse the last cell's key varint), and assign each record to
the first new page whose last rowid ≥ record rowid (rightmost page takes the
remainder — its upper bound is the parent's, not its last cell). Buffered
INSERT rowids may exceed every existing cell's rowid; they belong to the
rightmost page. Get-or-create the mapping entries for the target leaves and
insert the records. If any allocation fails mid-way, fall back to the
pending-list path for the remainder. Keep 1.4a as the error path forever.

### Stage 1.5 — Adapt flush, scan-prep, rollback, page lifecycle

1. **Flush** (`bfFlushOneMiniPage` + `bfFlushAllCallback`): now iterate ALL
   map entries whose stored root pgno matches the table; for each dirty
   mini-page: detach from map first (ownership rule from 1.4), replay dirty
   records through the temp cursor, then either re-attach with records
   marked clean (INSERT→CACHE, DELETE→PHANTOM) keyed by... the leaf may have
   split during replay, so re-attachment by old pgno can be WRONG. Simplest
   correct policy for this phase: **after a successful replay, discard the
   slab entirely** (lose the clean-retention optimization; re-promotion will
   repopulate). Partial failure: re-attach to the old pgno with only the
   failed dirty records retained is unsafe for the same reason — instead
   push the failed records through the pending-list mechanism. Record the
   simplification in a comment.
2. **Scan-prep** (`sqlite3BfBtreePrepareForScan`): still "flush every dirty
   mini-page of this table before any scan" — same visibility argument as
   before, now per-leaf granular. Keep both call sites around `moveToRoot`.
3. **ClearCache / rollback**: iterate map entries by root pgno; ALSO free
   the pending-rebalance list. Savepoint-open pre-flush stays as is.
4. **Page lifecycle**: a leaf can be freed (`freePage2`,
   `src/btree.c:6970`) when a table shrinks or is dropped — hook it: if the
   freed pgno has a map entry, dirty records there are impossible if flush
   ordering is right (assert in debug; in release, push to pending list),
   clean entries are dropped. `relocatePage` (`src/btree.c:3965`, autovacuum)
   moves a page to a new pgno: re-key the map entry (remove + insert) — or,
   simpler and acceptable for the thesis: **declare autovacuum unsupported
   with BF active** (refuse to enable BF record caching when
   `pBt->autoVacuum` is set, with a comment) and skip the relocatePage hook.
   Choose the simple option; document it in BF_TREE_DESIGN.md.
5. `sqlite3BfCacheTruncate` / `DROP TABLE`: dropping a table frees its
   pages page-by-page through `freePage2`, so the hook from (4) covers it.
   Verify with a stress seed that does heavy CREATE/DROP.

### Stage 1.6 — Restore the descent shortcut (the I/O win)

Now make BF hits skip the leaf-page read again — this is what the
larger-than-memory result depends on. In `sqlite3BtreeTableMoveto`'s descent
loop, just before `moveToChild(pCur, chldPg)` is called: probe
`sqlite3BfMapLookup(pBf, chldPg)`. Mini-pages exist only for leaves, so a
hit with `locType==BF_LOC_MINI` means `chldPg` is a leaf whose mini-page may
answer without loading the page:

- Search the mini-page for the encoded key. INSERT/CACHE hit → record
  found; DELETE/PHANTOM hit → record definitively absent. In both cases set
  the cursor to the state the existing per-table short-circuit used
  (cursor invalid-but-answer-known; mirror exactly what the current
  `bf_status` handling at `src/btree.c:6053` does with `*pRes` and cursor
  state, including `pCur->info.nKey`) and return WITHOUT loading the leaf.
- Miss in the mini-page → fall through to `moveToChild` normally.

Constraints: only when `bBypassActive==0` and not during scans
(`bMergingActive==0`); only intkey cursors; the payload path
(`sqlite3BfBtreeFetchPayload`) must work for a cursor in this
answer-known state — it already does for the per-table design; re-verify.
Subsequent cursor ops must re-seek (existing invalidation semantics).

### Stage 1.7 — Capacity & promotion retune + measure

With per-leaf capacity the 5% promotion rate is no longer the right default
guess. Do a small sweep (1/5/25/100%) on `point-zipf` and `ltm-read`; leave
the default at the best `ltm-read` value and note the sweep in the results.
Run the full Phase 0 matrix → `bench/results/phase1-perleaf/`. Expected
shape: `point-zipf` ≥ baseline, `ltm-read` hit rate way up, `mixed` still
poor (scan flush remains), `insert-batch` modestly up (buffer no longer
caps at 4 KB/table).

### Phase 1 acceptance
- [ ] Differential stress ≥ 18 seeds clean, including a seed with heavy
      splits (bulk random inserts of 1–2 KB payloads), heavy deletes
      (tree shrink), CREATE/DROP churn, and savepoint rollbacks.
- [ ] Assert-clean under `SQLITE_DEBUG` on the whole stress suite
      (pending-list empty at commit/close; no dirty record dropped).
- [ ] `veryquick.test` regressions: none vs Phase 0 record.
- [ ] Phase 1 CSVs committed; observations written to
      `bench/results/phase1-perleaf/NOTES.md`.

---

## Phase 2 — Merge-iterating scans + write-back deletes (~3 weeks)

Goal: scans consult mini-pages natively, so the scan-triggered flush
disappears (the biggest real-world confound) and deletes can become
write-back like the paper.

**This phase has the highest cursor-state complexity. Implement 2.1 fully
and keep it as the permanent fallback: any leaf whose mini-page situation
the merge code can't handle gets flushed the old way.**

### Stage 2.1 — Scoped scan flush (fallback + stepping stone)

Change `sqlite3BfBtreePrepareForScan` from "flush all dirty mini-pages of
the table, always" to a per-cursor *mode flag*: the scan registers that it
will use merge-iteration (Stage 2.2) and skips the flush; if at any point
the merge path bails (`pCur->bfScanFallback=1`), the cursor aborts to a
flush-then-restart: flush the table's dirty mini-pages, re-seek the cursor
to its saved position (`sqlite3BtreeTableMoveto` by last returned rowid),
continue as a plain scan. Until 2.2 exists, the flag is always-fallback —
behavior identical to Phase 1. Verify stress, commit.

### Stage 2.2 — Merge iteration in btreeNext/btreePrevious

Add to `BtCursor` (under the BF ifdef): the current leaf's mini-page pointer
(re-looked-up on every leaf transition — NEVER cached across operations,
eviction or rebalance can free it; re-validate via map lookup each time),
an index `bfIx` into the mini-page's sorted meta array, and a 1-bit "cursor
currently ON a mini-page record" flag.

Merge rules for forward iteration on one leaf (mini-page meta array is
sorted by 8-byte-BE key, which equals rowid order — same order as leaf
cells; assert both orderings in debug):

| Comparison (mini key vs next base cell key) | Mini op | Action |
|---|---|---|
| mini < base | INSERT | emit mini record, `bfIx++` |
| mini < base | DELETE/PHANTOM | skip (absent), `bfIx++` |
| mini < base | CACHE | emit mini record (equals an existing row only if base order is broken — assert it's emitted-as-extra never; in practice CACHE keys < next base cell but ≥ previous mean the base row was on a previous page — treat CACHE like INSERT for emission only if you chose to retain clean slabs; with Phase 1's discard-on-flush policy a CACHE key always matches SOME base cell — see next row) |
| mini == base | INSERT/CACHE | emit MINI version (shadow/update), advance both |
| mini == base | DELETE | skip both (row deleted), advance both |
| mini == base | PHANTOM | impossible (phantom = confirmed absent) — assert; in release emit base |
| mini > base | — | emit base cell, advance base |

End-of-leaf: when base cells are exhausted, drain remaining mini records
(< the leaf's upper bound, which is everything left in this mini-page);
when both exhausted, transition to the next leaf as stock code does, then
re-probe the map for the new leaf.

Payload for an emitted mini record comes from the mini-page heap (route
through `sqlite3BfBtreeFetchPayload`-style copy); `sqlite3BtreeIntegerKey`
must return the mini record's rowid when the on-mini flag is set.

**Bail-out triggers (set `bfScanFallback` and restart per 2.1)** — keep this
list in a comment at the top of the merge code:
- any write through ANY cursor on the same table during the scan
  (hook the existing cursor-invalidation walk `invalidateAllOverflowCache` /
  `saveAllCursors` path: a save/invalidate that touches a merging cursor
  triggers fallback);
- `saveCursorPosition` called while on a mini record (savepoint, statement
  boundary, concurrent BtreeInsert) — restoring to a key that exists only
  in a mini-page is exactly the Phase 1 Stage 1.6 lookup, so this CAN be
  made to work, but do not attempt it first; bail instead, measure, and
  only implement restore-to-mini if the fallback rate on `mixed` is > a few
  percent (report the rate in NOTES.md);
- mini-page eviction/rebalance between steps (detected by the re-lookup
  returning NULL or a different slab — this is why no caching across ops);
- reverse iteration (`btreePrevious`): either implement symmetrically or
  bail; implementing forward-only first is acceptable, but then ORDER BY
  ... DESC stress cases must exercise the fallback.

`sqlite3BtreeCount` (`src/btree.c:10728`) and `sqlite3BtreeLast`
(OP_NewRowid path! — `src/btree.c:5878`/`6520`): Last must see buffered
inserts with maximal rowids. Merge-aware Last is easy to get wrong; keep the
pre-flush for `Last` and `Count` initially (they're rare), note as future
work.

### Stage 2.3 — Write-back deletes

Once scans merge tombstones, `sqlite3BtreeDelete` can buffer instead of
write-through, under gating symmetric to inserts: rowid table, no
`BTREE_AUXDELETE` complications — **study the call site first**: index
entries are deleted alongside table rows; the table-row delete may be
buffered but the index-entry delete must still happen (indexes have no BF).
Buffer the tombstone (`BFOP_DELETE`) for the table row, skip the base
delete, invalidate the cursor. The flush replay already handles DELETE
records (`bfApplyOneRecord` does moveto+delete; verify it treats
"key not found in base" as success — the row may have existed only as a
buffered INSERT that the tombstone cancelled inside the mini-page).

Edge cases: delete of a row that exists only as a buffered INSERT must net
to "no row" both in scans and after flush (the mini-page in-place overwrite
gives this — assert with a targeted test); `OP_NewRowid` / `BtreeLast`
interaction (a deleted max-rowid must NOT make Last return it — Last
pre-flushes, which applies the tombstone first; keep that ordering);
`changes()` counting must still report the delete.

If Stage 2.2's fallback rate makes write-back deletes net-negative on
`mixed`, keep deletes write-through behind a compile-time flag and report —
that is a legitimate ablation finding, not a failure.

### Stage 2.4 — Measure

Full matrix → `bench/results/phase2-mergescan/`. Key result: `mixed` should
move from regression to ≥ parity with Config B; report the fallback rate.

### Phase 2 acceptance
- [ ] Differential stress extended with: scans interleaved with writes on
      the same connection, ORDER BY DESC scans, DELETE-heavy mixes,
      `SELECT count(*)` mid-transaction, INSERT→DELETE→scan of same key in
      one transaction. ≥ 18 seeds clean.
- [ ] Fallback path provably exercised (debug counter > 0 across suite) and
      clean.
- [ ] Phase 2 CSVs + fallback-rate notes committed.

---

## Phase 3 — Eviction-driven merge + copy-on-access (~2–3 weeks)

Goal: close the last two reachable divergences — dirty slabs at the buffer
head no longer stall reclamation, and hot data self-preserves like the
paper's copy-on-access.

### Stage 3.1 — Strip dead concurrency machinery (cleanup)

Per the permanent-divergence decision: remove the per-entry rwlock fields
and `sqlite3BfMapLock*` functions (`src/bf_mapping.c`), and reduce the
allocation-state atomics to plain stores with a comment ("serialized by
BtShared") IF the state machine remains correct single-threaded — keep the
state VALUES (the lifecycle is still real), just drop CAS/backoff loops.
Mechanical, but do it as its own commit with stress verification: it
simplifies everything after.

### Stage 3.2 — Flush-capable eviction

In `evictCallback` (`src/bf_cache.c:951`), a mapped+dirty slab currently
aborts the sweep. Add a guarded merge path:

- New `BfCache` flag `bInBtreeOp`, set/cleared at the entry/exit of the
  outermost public btree mutators and movetos that can allocate from the
  buffer (BtreeInsert, BtreeDelete, the moveto hooks). When
  `bInBtreeOp!=0` (eviction triggered from inside a btree operation) →
  refuse as today. The interesting case — eviction pressure during reads or
  from pcache2 stress — has `bInBtreeOp==0` and may flush.
- When allowed: resolve the owner table root from the slab metadata
  (Stage 1.1), obtain the `Btree*` via `pBf->pBtShared` (validate non-NULL;
  refuse if NULL), and run the Phase 1 flush ("detach, replay, discard")
  for that one mini-page, then reclaim. All §1.3 guards apply
  (`bBypassActive` etc.). Any error → refuse (abort sweep), never half-free.
- Keep a stat counter `nEvictMerges`; expose in `bf_cache_stats`.

Trap: the flush replays through `sqlite3BtreeInsert`, which can allocate
mini-pages... it cannot — `bBypassActive` makes all BF write hooks no-ops,
so replay never allocates from the buffer. Assert
`bBypassActive==1 → sqlite3BfRecordWrite refuses` (it already does; keep it
true).

### Stage 3.3 — Copy-on-access on reads

In the read-hit paths (the Stage 1.6 descent probe and
`sqlite3BfBtreeFetchPayload`), after a hit on slab S: if
`sqlite3BfCircularBufferIsCopyOnAccess(&pBf->cb, S)` (S is near the head):

1. Read the answer out FIRST (copy payload to the caller's buffer).
2. Allocate a same-size slab at the tail. The allocation may trigger an
   eviction sweep that reclaims S — harmless, the answer is already out,
   but the map entry may now be gone: re-lookup the entry; if it no longer
   points at S, skip the copy (someone else won).
3. `memcpy` S's contents to the new slab, `sqlite3BfMapUpdateLocation` the
   entry to the new slab, dealloc S (which, in the head region, tombstones
   rather than freelists — existing behavior, correct here).
4. If allocation fails, skip silently — copy-on-access is best-effort.

Single-threadedness (BtShared) is what makes this safe; say so in the
comment. Add stat `nCopyOnAccess`.

### Stage 3.4 — Promotion-policy experiment + final measurement

With eviction-merge and copy-on-access in place, promote-always may now be
viable (the paper's behavior). Sweep promotion rate {1, 5, 25, 100} ×
{`point-zipf`, `point-uniform`, `ltm-read`, `mixed`} → this sweep is itself
a thesis results section. Then the full matrix →
`bench/results/phase3-final/`, plus a consolidated ablation table
(baseline → phase1 → phase2 → phase3) per workload, with hit rates.

### Phase 3 acceptance
- [ ] Stress suite (all extensions from Phases 1–2) ≥ 18 seeds clean, plus
      a small-buffer torture config (`PRAGMA bf_cache_size` = 256 KB) so
      eviction-merge and copy-on-access fire constantly (assert
      `nEvictMerges>0`, `nCopyOnAccess>0` after the run).
- [ ] No dirty-head stall: a workload that previously degraded to
      permanent fallback under a tiny buffer now sustains buffering (compare
      `nEvictions`/`nEvictMerges` before/after).
- [ ] Final ablation table committed; BF_TREE_DESIGN.md updated: §4, §5.3,
      §6.2, §9, §12 all describe the OLD design — rewrite the affected
      sections and refresh the divergence table (remaining divergences
      should be exactly: durability, concurrency, plus any documented
      simplifications like autovacuum-off and Last/Count pre-flush).

---

## Appendix A — Quick reference: current hook sites (verify before editing;
line numbers drift)

| Site | Location | Phase that touches it |
|---|---|---|
| Savepoint-open flush | `src/btree.c:3613` | 1.5 |
| Commit flush | `src/btree.c:4343` | 1.5 |
| Rollback ClearCache | `src/btree.c:4561`, `4680` | 1.5 |
| Payload fetch + promotion | `src/btree.c:5391`, `5405`, `5442` | 1.2, 3.3 |
| Scan-prep calls | `src/btree.c:5776`, `5781`, `5878`, `6520`, `10728` | 1.5, 2.1, 2.2 |
| TableMoveto exists/phantom | `src/btree.c:6053`, `6063` | 1.2, 1.6 |
| Insert buffering + fallthrough cache | `src/btree.c:9640`, `9900` | 1.3 |
| Delete tombstone | `src/btree.c:10092` | 1.3, 2.3 |
| balance_nonroot / balance_deeper | `src/btree.c:8379`, `9183` | 1.4 |
| freePage2 / relocatePage | `src/btree.c:6970`, `3965` | 1.5 |
| Flush machinery | `src/bf_btree.c:104–234` | 1.5, 3.2 |
| Eviction sweep + callback | `src/bf_cache.c:920–990` | 1.1, 3.2 |

## Appendix B — When you are stuck

- If a stage's optimization can't be made stress-clean in ~2 days of
  effort, ship the documented fallback and move on; record the gap in the
  phase NOTES.md. The ablation methodology tolerates a missing
  optimization; it does not tolerate a correctness regression or a
  schedule collapse.
- If differential stress diverges and the cause is unclear: bisect by
  disabling features via the existing pragmas (`bf_cache=OFF` first — if
  divergence persists, the bug predates your change), then by stage
  commits. `SQLITE_BF_DEBUG` traces to `build/bf-allocs.log` show the
  allocation/map history.
- Phases must land in order. Within a phase, stages must land in order.
