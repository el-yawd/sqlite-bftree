# Persisted mini-pages: checkpoint without writing every dirty leaf back

Status: **PROPOSAL, awaiting owner approval** (2026-10-13). WAL mode only.
Reopens one locked decision (section 9). Branch: `writes-commit-path`.

## 1. Problem

Before every checkpoint, `bfCheckpointMaterialize` (btree.c) runs a write transaction
that merges every buffered record into its base leaf. Each dirty leaf becomes a 4 KB
WAL page frame, and the checkpoint then copies it into the db file. The cost scales
with the number of dirty leaves, not with the bytes that changed.

Measured (1-row UPDATE commits, 100k rows, default autocheckpoint):

| | per commit |
|---|---|
| stock | 3.25 µs |
| BF | 10.19 µs, of which materialization at checkpoint is 3.4 µs |

On that workload a typical checkpoint writes about 2,500 leaves to carry about 2,500
changed rows, roughly one row per 4 KB page.

The leaves are written back because the WAL reset after a checkpoint discards the
record frames, so the records must be durable somewhere else first. The reference
never resets its WAL. Its CPR snapshot writes the mini-pages, unmerged, to a separate
snapshot file (`../bf-tree/src/snapshot.rs`).

## 2. Idea

Do what the reference's snapshot does, inside the db file and through SQLite's own
transactions. At checkpoint, write each changed leaf's **dirty records** into a row of
a hidden table, instead of merging them into the leaf. The db file is then complete
(base pages plus stored records), so the WAL reset is exactly as safe as in stock.
Recovery and reopen load the stored records back into the cache as buffered records.

## 3. Why a hidden table, not a new page type

A raw "mini-page page" type would need its own free-space management, its own
integrity-check support (otherwise `PRAGMA integrity_check` reports "Page N is never
used"), autovacuum relocation handling, and parsing changes in btree.c.

A rowid table gets all of that for free:
- B-tree packing, about 25 rows of ~150 B per page;
- the freelist;
- integrity checking;
- crash atomicity through the WAL.

Its pages are ordinary table pages, so stock SQLite can still open the file.

## 4. On-disk format

```
CREATE TABLE sqlite_bf_store(leaf INTEGER PRIMARY KEY, ops BLOB)   -- internal, like sqlite_stat1
```

- `leaf`: the user-table leaf the records are keyed to (the cache's map key).
- `ops`: that leaf's **dirty** records, encoded as a WAL record-batch payload
  (`bf_wal.c` codec, with rootPgno per op). This reuses the decoder, its fuzz and
  ESBMC coverage, and the replay code. Clean records are a cache and are not stored.
- The table is created lazily by the first persist, which bumps the schema cookie
  once. It is never buffered by BF itself: its writes are page-path only.

New WAL op `BFWAL_OP_PERSIST` (payload v5), logged once per persisted leaf in the
persist commit. It means "this leaf's records up to here are in its `sqlite_bf_store`
row". It is the counterpart of `CLEAR`, which means "up to here they are in base
pages".

## 5. Invariants

- **I1 (durability).** Every committed record that is not in base pages can be
  reconstructed from two sources, in order: the leaf's store row as of the snapshot,
  then the leaf's record ops after its last `PERSIST` or `CLEAR` marker that is
  ≤ mxFrame. This replaces today's rule ("base pages, then ops after the last CLEAR").
- **I2 (merge deletes the row).** Every merge of leaf L deletes row L **in the same
  transaction** that logs L's `CLEAR`. This covers merge-on-full, merge-on-evict, the
  table sweep, commit-time flush, and materialize. So a row can never be older than a
  CLEAR, and replay never has to choose between the two.
- **I3 (completeness).** Every row is in every caught-up cache as a dirty mini-page.
  This already holds for replayed records. Replay-fit (`ring_grow`, fail loudly with
  `SQLITE_FULL`) covers capacity.

## 6. Checkpoint flow (replaces `bfCheckpointMaterialize` in WAL mode)

1. Begin a write transaction. As today, any connection may run it: its cache is
   complete for the latest snapshot. Stage any open deferred-commit batch first, as
   materialize does (D2 ordering).
2. The leaves to persist are those with record ops after their last marker in the
   connection's WAL index. This is connection-independent: no per-cache "changed"
   bits are needed.
3. For each such leaf, apply the policy (section 7). Either:
   - **persist**: upsert the row with the leaf's full current dirty set, and log `PERSIST(L)`; or
   - **merge**: today's flush, which logs `CLEAR(L)` and deletes row L.
4. Commit. Stock backfill follows, and a later writer resets the WAL as usual.

Records stay dirty in the cache after a persist. They are merged later by
merge-on-full, merge-on-evict or close, exactly as now.

## 7. Policy: persist or merge

A merge writes one 4 KB leaf. A persist writes about the leaf's dirty bytes, plus a
share of a store page. Rules:

- Merge when the leaf's dirty bytes reach `PRAGMA bf_persist_max` (default 1024 B,
  about a quarter page; to be swept).
- Otherwise persist.

The store size is bounded by the dirty set, which is bounded by the ring budget.

## 8. Replay paths (every one changes)

| path | today | new |
|---|---|---|
| recovery / reopen | ops after the last CLEAR | scan `sqlite_bf_store`, then ops after the last marker |
| rollback rehydrate | same as recovery, plus carry | same, plus carry |
| reader full rebuild | same as recovery | same as recovery |
| reader incremental | drop touched leaves, replay their ops | also re-read the touched leaves' store rows (a point lookup). Store-table page frames and `PERSIST` ops name the leaves, so the touch list is unchanged |
| replay bound (`ring_grow`) | ops after the last CLEAR | plus row sizes |

Reading the store table during replay means reading pages inside the replay. Recovery
and rehydrate already run with the pager readable, so this is the main implementation
risk to check first.

## 9. Lifecycle and locked decisions

- **Close: unchanged.** Close still materializes everything and runs a TRUNCATE
  checkpoint, then deletes all rows. So **a db at rest is a plain SQLite file**: stock
  SQLite sees an empty `sqlite_bf_store` table. Only a crashed or still-open db holds
  stored records.
- **journal_mode away from WAL, VACUUM:** merge all first (I2 empties the store).
- **`DROP TABLE` / `DELETE FROM`:** these already flush the table first, so I2 deletes its rows.
- **Autovacuum:** BF is already off under autovacuum (`bf_btree.c:75`), so no page relocation.
- **Locked decision reopened:** "the only BF data on disk is in WAL record frames"
  becomes "...or in `sqlite_bf_store` while the db is open or after a crash". The other
  locked decisions hold: one WAL, page-sized frames, no sidecar file, single writer.
  Stock code is not modified to fit this; the stock baseline is untouched.

## 10. Crash cases (new oracle coverage)

Kill (SIGKILL) and power-loss (the existing powerloss harness) at each point:

1. before the persist commit;
2. after the persist commit, before backfill;
3. mid-backfill;
4. after the reset, inside the first new-generation commit;
5. inside a merge that deletes a row;
6. a reader rebuilding across a persist and a reset.

Expected outcome in all cases: committed records survive, and the differential oracle
matches stock. Cases 1–3 rely on the old log being intact. Case 4 relies on the db file
holding the rows. Case 5 relies on transaction atomicity.

## 11. Measurement

- Ablation: `SQLITE_BF_NO_PERSIST` (today's materialize).
- Counters: `persist_rows`, `persist_bytes`, `persist_merges`, `store_pages`.
- Report WAL page frames per checkpoint and µs per commit beside the existing write
  arms, under direct I/O (`28d0d2b`).
- Expected: page frames per checkpoint go from about the dirty-leaf count (~2,500) to
  about store pages touched (~100) plus merged leaves. Checkpoint cost per commit drops
  from 3.4 µs toward stock's backfill cost.
- Not expected: any change to the commit-path gap (about 1.8x instructions, plan
  2026-10-13).

## 12. Stages

1. **S1 (~1.5 days).** Store table, payload v5 `PERSIST`, persist transaction, recovery
   load. Crash cases 1–4 under the single-connection oracles.
2. **S2 (~1 day).** I2 in every merge path. Rollback, reader full and incremental
   replay. mconn sweep. Crash cases 5–6.
3. **S3 (~1 day).** Policy sweep, counters, ablation, gate variants, a
   `results/persist` campaign.

## 13. Open questions for the owner

1. Is reopening the "BF data only in WAL record frames" decision acceptable? The db at
   rest stays a plain SQLite file.
2. Is the table name and visibility OK? It is listed in `sqlite_schema`, like `sqlite_stat1`.
3. Policy default: should the threshold be swept in the campaign, or fixed now?
