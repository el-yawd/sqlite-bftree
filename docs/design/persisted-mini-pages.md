# Persisted mini-pages: a native page type, so a checkpoint stops writing every dirty leaf back

Status: **SUPERSEDED** by `write-path.md` (2026-10-13) -- kept for the reasoning. WAL mode only.

v2 replaces v1's hidden-table design. Owner: "modifying the base file format is the
correct way to go, fuck compatibility".

## 1. Problem

Before every checkpoint, `bfCheckpointMaterialize` (btree.c) merges every buffered
record into its base leaf. Each dirty leaf is written as a 4 KB WAL page frame and
then copied into the db file. The cost scales with the number of dirty leaves, not the
bytes that changed.

Measured (1-row UPDATE commits, 100k rows, default autocheckpoint):

| | per commit |
|---|---|
| stock | 3.25 µs |
| BF | 10.19 µs, of which materialization at checkpoint is 3.4 µs |

A typical checkpoint writes about 2,500 leaves to carry about 2,500 changed rows.

The leaves are written back because the WAL reset after a checkpoint discards the
record frames. The reference never resets its WAL; its CPR snapshot writes the
mini-pages, unmerged, to a separate file (`../bf-tree/src/snapshot.rs`).

## 2. Idea

Make mini-pages part of the database file format. A new page type, the **mini-page
page** (`MP`), packs the dirty records of many leaves. At checkpoint, the changed
leaves' dirty records are written into MP pages instead of being merged into their
leaves.

MP pages are ordinary database pages, written through the WAL like any other page. So
after the checkpoint the db file is complete (base pages plus MP pages), and the WAL
reset is exactly as safe as in stock. On open or recovery, the MP pages are loaded back
into the cache as buffered records.

## 3. File format changes

**Database header**
- Bytes 18/19 (read/write format version) become 3. Stock SQLite then refuses to open
  the file, rather than silently reading leaves that are missing their buffered rows.
- Bytes 72–75 (currently unused): **MP head**, the pgno of the newest MP page, or 0.
- Bytes 76–83: **persist epoch**, a u64 incremented by every persist.

**MP page** (page type byte `0x10`, which SQLite does not use)
```
offset 0   u8   type = 0x10
       1   u8   flags
       2   u16  nSlot
       4   u16  first free byte
       6   u32  next MP page (older), 0 = end of chain
      10   u64  epoch of the persist that wrote this page
      18   ...  slots, packed
slot:  u32 leaf | u32 root | u16 len | u8 kind (0 = records, 1 = tombstone) | len bytes
```

The slot body is the leaf's dirty records in the WAL record-batch encoding (`bf_wal.c`).
This reuses its decoder, its fuzz and ESBMC coverage, and the replay code. Clean records
are a cache and are not stored. A capped mini-page (2,048 B) always fits one page.

**Chain semantics.** Each persist writes a new **segment**: one or more MP pages linked
in front of the chain, carrying the same epoch. Walking the chain from the head, the
first slot seen for a leaf is that leaf's current state. That slot is either its records
or a tombstone ("records merged into the leaf since").

**Compaction.** When chain pages exceed 2× the pages needed for the live slots, the
persist rewrites the live slots into a fresh segment and frees the old pages to the
ordinary freelist.

**Integrity check** learns the MP chain: pages reachable from the MP head are in use,
and their slots must decode.

## 4. New WAL op

`BFWAL_OP_PERSIST` (payload v5) is logged once per persisted leaf, in the persist commit.
It means "this leaf's records up to here are in the MP chain". It is the counterpart of
`CLEAR`, which means "up to here they are in base pages".

## 5. Invariants

- **I1 (replay).** Leaf L's dirty set at a snapshot is built as follows:
  1. Take L's current slot in the MP chain as of that snapshot, ignoring it if it is a
     tombstone, or if a `CLEAR(L)` in the log is newer than that slot's `PERSIST(L)`.
  2. Then apply L's record ops after its last marker (`CLEAR` or `PERSIST`) that is
     ≤ mxFrame.

  Today's rule is the special case with an empty chain.
- **I2 (tombstones before a reset).** A merge only logs `CLEAR(L)`, as today, with no
  extra write. The next persist writes a tombstone for every leaf `CLEAR`ed since the
  previous persist. Since a reset follows a persist, no stale slot can outlive the
  `CLEAR` that killed it.
- **I3 (no reset with unpersisted records).** `walRestartLog` (our wal.c) refuses to
  reset while a record op or `CLEAR` is newer than the last persist commit. That
  happens when a commit lands between the persist and the backfill. The log then keeps
  growing until the next checkpoint persists. The writer's WAL index is caught up when
  it reaches `walRestartLog`, so the check is local.
- **I4 (completeness).** Every live slot is in every caught-up cache as a dirty
  mini-page. Replay-fit (`ring_grow`, fail with `SQLITE_FULL`) covers capacity.

## 6. Checkpoint flow (replaces `bfCheckpointMaterialize`)

1. Begin a write transaction. As today, any connection may run it. Stage any open
   deferred-commit batch first (D2 ordering).
2. Find the leaves with record ops after their last marker (from the WAL index;
   connection-independent), plus the leaves `CLEAR`ed since the last persist.
3. Apply the policy (section 7) to each changed leaf:
   - **persist**: write a slot with the leaf's full current dirty set, and log `PERSIST(L)`; or
   - **merge**: today's flush, which logs `CLEAR(L)` and gets a tombstone.
4. Write the segment, update the MP head and epoch in page 1 (dirty anyway in every
   commit), compact if due, and commit.
5. Stock backfill follows; a later writer resets the WAL (subject to I3).

Records stay dirty in the cache. They are merged later by merge-on-full, merge-on-evict
or the policy.

## 7. Policy: persist or merge

A merge writes a 4 KB leaf. A persist writes the leaf's dirty bytes into a shared MP
page. Merge when the dirty bytes reach `PRAGMA bf_persist_max` (default 1024 B, to be
swept); otherwise persist. The chain's live size is bounded by the dirty set, which is
bounded by the ring budget.

## 8. Replay paths

| path | change |
|---|---|
| recovery / reopen | walk the MP chain (newest first, first slot per leaf wins), then apply ops after the markers (I1) |
| rollback rehydrate | same, plus carry |
| reader full rebuild (log reset) | same as recovery; reads the MP chain at its snapshot |
| reader incremental | a persist changes no record values, so a `PERSIST(L)` is just a marker and needs no reload; `CLEAR` handling is unchanged |
| replay bound (`ring_grow`) | plus live slot sizes |

Main implementation risk, to check first: the replay reads MP pages through the pager
while it rebuilds the cache.

## 9. Lifecycle

- **Close: persist and checkpoint, with no write-back.** That removes today's close
  burst. A db at rest holds MP pages and needs this engine to open.
- **journal_mode away from WAL, VACUUM:** merge everything first, which empties the chain.
- **`DROP TABLE` / `DELETE FROM`:** these already flush the table first, so its leaves
  get tombstones at the next persist.
- **Autovacuum:** BF is already off under autovacuum (`bf_btree.c:75`), so MP pages are never relocated.
- **Locked decisions:**
  - Reopened: "BF data on disk only in WAL record frames" becomes "...and in MP pages".
  - Kept: one WAL, page-sized WAL frames, no sidecar file, single writer.
  - Fairness: the stock baseline is not modified; this is the BF engine's own format.

## 10. Crash cases (new oracle coverage)

Each under SIGKILL (crash oracle) and power loss (powerloss harness):

1. before the persist commit;
2. after it, before backfill;
3. mid-backfill;
4. after the reset, in the first new-generation commit;
5. a commit between the persist and the backfill (I3 must block the reset);
6. compaction;
7. a reader rebuilding across a persist and a reset.

Pass condition: the differential oracle matches stock, and `integrity_check` (with MP
support) is ok.

## 11. Measurement

- Ablation: `SQLITE_BF_NO_PERSIST` (today's materialize).
- Counters: `persist_slots`, `persist_tombstones`, `persist_merges`, `mp_pages`, `mp_compactions`.
- Report WAL page frames per checkpoint and µs per commit beside the write arms, under
  direct I/O.
- Expected: page frames per checkpoint go from about the dirty-leaf count (~2,500) to
  about the MP pages written (~2,500 × ~150 B ≈ 90) plus merged leaves.
- Not expected: any change to the commit-path gap (~1.8x instructions).

## 12. Stages

1. **S1 (~2 days).** MP page format, header fields, format version, persist
   transaction, chain load at recovery, integrity check. Crash cases 1–4 on the
   single-connection oracles.
2. **S2 (~1.5 days).** I1/I2/I3 in every replay path (rollback, reader full and
   incremental), the reset guard, compaction. mconn sweep. Crash cases 5–7.
3. **S3 (~1 day).** Policy sweep, counters, ablation, gate variants, a `results/persist` campaign.

## 13. Open questions

1. Close persists and does not write back, so a db at rest needs this engine to open. Is that OK?
2. Should the slot store only dirty records (proposed), or the whole mini-page image
   (the reference snapshots raw mini-pages)? Whole images would also keep clean records
   across a reopen (a warm restart), at about 2–4x the bytes.
3. Policy default: sweep it in the campaign, or fix it now?
