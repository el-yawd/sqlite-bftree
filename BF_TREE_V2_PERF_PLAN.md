# Phase 3 performance plan — fixing the three write-path bottlenecks

Written 2026-09-08, after profiling the `fixes` campaign build. Every item below
names the evidence that motivates it and the place in `../bf-tree/` that says
what the reference implementation does, because the null hypothesis in this
project is that a slowdown is **our integration**, not a non-transferable paper
result.

## The measurements this plan responds to

| # | finding | evidence |
|---|---|---|
| 1 | `bfCacheTruncate` scans the whole page cache on **every commit** | 50.3% of write cycles; insert rate collapses 36,153 → 1,512 ops/s as the page cache grows 8 → 128 MiB with I/O removed (tmpfs). Stock on tmpfs at 128 MiB: 26,563 ops/s, no such shape |
| 2 | commit logs the **entire accumulated dirty set**, not the transaction's own pages | 24.9% of cycles in `sqlite3BfBtreeLogAllDirty → sqlite3BfDirtyListIterate`, with one row per transaction |
| 3 | every buffered insert faults in a 4 KiB **base page** | on btrfs, BF reads 3.9 KiB/op vs stock's ~100 B/op while writing 24x fewer bytes and running 31x slower (2,028 vs 62,566 ops/s) |
| 4 | write amplification regressed with the new size classes | `pg frames/commit` 1.00 → 3.74 at 64 B payloads; write volume 182 → 924 MiB |
| 5 | reads are at parity with stock, not ahead | same bytes read as stock (ratio 1.01) under the 1 GiB cgroup; warm/in-RAM BF is 0.91x stock |

## What the reference does (`../bf-tree/`)

* **No per-commit cache sweep exists at all.** Bf-Tree has no pager and no
  `xTruncate` hook; item 1 is 100% integration overhead we added, and SQLite's
  own `pcache1TruncateUnsafe` (`src/pcache1.c`) already shows the cheap way to
  do it.
* **The WAL appends one record per mutation.** `Wal::append_and_wait`
  (`src/wal/mod.rs:196`) copies the single `write_op` into a shared buffer and
  waits for its LSN; a background thread flushes. Cost per mutation is O(1) and
  nothing ever walks a dirty set. Item 2 is ours.
* **An insert never reads the base page.** In `MiniPageOp::insert`
  (`src/mini_page_op.rs:474`), a leaf at `PageLocation::Base(offset)` gets a
  *newly allocated mini-page* with `next_level = offset` and the record written
  blind; the base page is read only for the root leaf, for CPR snapshots (a
  feature we do not have), and when a mini-page must be merged back. Item 3 is
  ours, and it defeats the point of write buffering.
* **Outgrowing the size classes triggers merge + full-page cache, not a
  per-insert fallback.** `new_size_if_upgrade` (`src/nodes/leaf_node.rs:1713`)
  returns `None`, the caller merges the mini-page into the base page and, under
  `write_load_full_page` (default `true`, `src/config.rs:155`), promotes the
  leaf to a whole-page cache entry. Relevant to item 4.
* **The ring already tries the free list first**, exactly as ours does
  (`src/circular_buffer/mod.rs:539` vs `sqlite3BfCircularBufferAlloc`), so the
  ~4 KiB-per-record occupancy is **not** a missing free list. The remaining
  suspect is our copy-on-access rejection tombstoning reusable blocks. To be
  measured, not assumed.

## Stages

Each stage is independently committable and ends with the same gate: the four
differential oracles, then the A/B benchmark. Stages are ordered by
(evidence strength x safety) / cost.

### Stage 0 — close the correctness gate that is still open (blocking)

`sqlite3BfBtreeResizeCache` has never passed `stress_buf.sh`; it frees the ring
that live mini-pages sit in. Nothing below is trustworthy until this is green.

```sh
cd build && make sqlite3 && cc -O2 -DSQLITE_BF_INSERT_BUFFERING ... -o sqlite3_buf shell.c sqlite3.c -lm -lz
cd ../bench && sh stress.sh && sh stress_buf.sh \
  && BF_PROMOTION=100 sh stress_buf.sh && BF_GROUP=8 sh stress_buf.sh
```

### Stage 1 — make `bfCacheTruncate` proportional to what it removes

`src/bf_cache.c`. Mirror `pcache1TruncateUnsafe`: track `iMaxKey` in
`BfCacheInt` (maintained where a page is added to the hash), and when
`iMaxKey - iLimit < nHash`, scan only buckets `iLimit % nHash` through
`iMaxKey % nHash` instead of all of them. Fall back to the full sweep when many
pages are being dropped, exactly as upstream does.

* **Risk:** low. Pure hash-walk bookkeeping; a missed page would leak a stale
  cache entry above the truncation point, which the oracles exercise directly
  (rollback and vacuum both truncate).
* **Expected:** removes ~50% of write-path cycles and, more importantly, the
  page-cache-size dependence. This is the one change that should move every
  write number in the campaign.
* **Verify:** re-profile (the method says re-profile, because the first
  `IsDirty` fix looked obviously right and moved nothing), then re-run the
  8/32/128 MiB page-cache sweep on tmpfs and check the collapse is gone.

### Stage 2 — scope commit-time logging to the transaction

`src/bf_btree.c` / `src/bf_cache.c`. Today `sqlite3BfBtreeLogAllDirty` iterates
every dirty mini-page in the cache. Add a **transaction-scoped** dirty list: an
array of pgnos dirtied since the write transaction opened, appended where
`BF_MINI_F_DIRTY` is first set, iterated at commit, cleared on commit/rollback,
with an overflow flag that falls back to today's full walk (the same
`bDirtyListOverflow` pattern already in `sqlite3BfDirtyListIterate`).

* **Risk:** medium — this is dirty-tracking, where a wrong answer silently loses
  writes. The overflow fallback keeps the failure mode conservative.
* **Expected:** commit cost drops from O(dirty set) to O(this txn's writes),
  removing the O(N²) shape of N single-row commits (~25% of cycles).
* **Not attempted:** moving to per-mutation logging like the reference's
  `append_and_wait`. That changes the WAL emission point and the recovery
  contract; a txn-scoped list gets the same asymptotics with the existing codec.

### Stage 3 — stop reading the base page on a buffered insert

The deep one, and the largest expected win on real storage (BF reads 3.9 KiB/op
against stock's ~100 B/op). **Revised 2026-09-15 after reading our insert path:
the reference's design does not drop in, and the reason is worth writing down.**

`../bf-tree/`'s `MiniPageOp::insert` can write blind because Bf-Tree's insert is
an upsert — it has no caller asking "did this key already exist?". SQLite's
`sqlite3BtreeInsert` does: `loc` comes from the descent
(`sqlite3BtreeTableMoveto`), and **that descent is what reads the leaf**. Worse,
today's buffering deliberately depends on `loc`: `btree.c` buffers only when
`loc!=0`, which is what makes "a mini-page INSERT for a key that also exists in
the base leaf" impossible and keeps the merge rules simple. Removing the leaf
read removes that guarantee, so the work is not one shortcut but four pieces:

a. **Let the descent stop at the parent for a write cursor.**
   `sqlite3BfBtreeDescentProbe` refuses write cursors today ("writes need the
   physical leaf"). The insert path does not actually need a positioned cursor
   on success — it already sets `CURSOR_INVALID` and returns — so what it needs
   is the child pgno, which the parent has.
b. **Buffer the record against that pgno without consulting the base leaf.**
c. **Teach the merge that a buffered INSERT upserts a base cell of the same
   key** — the invariant removed in (a)/(b). This is where the correctness risk
   concentrates, and it is what the reference does natively (a dirty INSERT wins
   over the base record).
d. Reads already prefer the buffered record, so nothing new there.

**Safe subsets available without (c),** worth measuring first because they are
nearly free:
  - `BTREE_APPEND` inserts: the caller guarantees the key is above every
    existing one, so novelty needs no leaf read.
  - keys for which the mini-page already holds a `BFOP_PHANTOM` (a previous
    probe proved absence).
Neither helps the benchmark much — `bfbench` inserts random keys into gaps, so
almost nothing is an append — which is precisely why the subsets should be
measured rather than assumed: if they cover a negligible share of inserts, that
is the evidence for paying for (c).

* **Risk:** high. Cursor invariants (`BTCF_BfLeaf`, `BTCF_ValidNKey` — the Phase
  1 scan bug lived here), splits, and the merge path, which still needs the base
  page.
* **Sequencing:** behind `SQLITE_BF_NO_BLIND_INSERT` (default = feature on) so
  one amalgamation measures both ways and a regression is one flag from being
  isolated. Add `blind_inserts` / `blind_refused` counters so the share of
  inserts that qualify is measured, not assumed.
* **Gate:** starts only once Stages 1-2 are committed green, so the deep change
  develops on a clean base.

### Stage 4 — explain, then fix, the size-class write amplification (item 4)

Measure before changing anything: instrument merges/evictions per insert at 64 B
and compare pre- and post-fix builds. Two candidate causes to separate:
mini-pages that are larger evict sooner (fewer fit in the ring), versus each
eviction merging into a base page that then goes to the WAL as a page image. The
reference's answer for a mini-page that cannot grow — merge and promote the leaf
to a whole-page cache entry — is the design to compare against, but only once
the data names the frame.

### Stage 5 — ring occupancy (item 5's precondition)

~4 KiB of ring per ~130 B record starves the record cache, which is why reads
are at parity rather than ahead. The free list is not the cause; instrument
`nFreeListHits` versus copy-on-access tombstoning first, then decide. Deferred
behind the write path because the write path has the bigger measured gap.

## Benchmark re-run protocol

The A/B setup already exists ([`bench/harness/README.md`](bench/harness/README.md)
sections 10-11): `build_suts.sh --pre REV` builds a `bf_pre` SUT from a second
amalgamation so before/after runs interleaved in one campaign, which matters
because identical code has measured 9913 / 11652 / 8102 ops/s across campaigns.

After each stage:

1. `sh bench/harness/build_suts.sh --pre <the commit before this stage>`
2. `python3 bench/harness/runner.py bench/harness/configs/fixes_smoke.json` (2 min plumbing)
3. `python3 bench/harness/runner.py bench/harness/configs/fixes.json` (~75 min)
4. `python3 bench/harness/report.py bench/harness/results/fixes`

Read the counters before the wall clock: `pg frames/commit`, `buffered%`,
`evictions`, `ring MiB`, and read/write bytes are governor-independent; the
governor here is `powersave` (changing it needs a password) and nine of the
eighteen write cells in the last campaign exceeded the 15% spread threshold.

One methodology fix to fold in: **keep scratch databases off tmpfs.** The first
reading of the 24x truncate collapse was taken on a tmpfs copy, where I/O is
free; on btrfs the same sweep is 2,028 → 1,368 ops/s. Both numbers are true and
they answer different questions, so state the storage with every write number.
