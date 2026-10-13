# Write path redesign: the record log lives in database pages

Status: **SUPERSEDED** by `concurrent-engine.md` (owner did not like it). Was: PROPOSAL (2026-10-13). WAL mode only. Compatibility is not a constraint
(owner); durability must stay at SQLite's level. Supersedes
`persisted-mini-pages.md` (v2), which this design generalises (section 9).

## 1. The gap

Workload: 1-row UPDATE commits, 100k rows of 100 B, in RAM. Ranges marked "est." are
estimates.

| | stock | BF today |
|---|---|---|
| µs/commit, default autocheckpoint | 3.25 | 10.19 |
| ... of which checkpoint write-back | ~0 | 3.4 |
| µs/commit, checkpoints off | 3.7-4.6 | 6.9 |
| WAL bytes/commit | 4,120 | 4,120 (record frame, ~97% zeros) |
| pages written back per 1,000 commits (est.) | ~850 (distinct pages backfilled) | ~850 leaves x2 (WAL frame + backfill) |

Today's checkpoint writes back every dirty leaf because a WAL reset would discard the
record frames (`btree.c` `bfCheckpointMaterialize`). That is the largest single cost.
The rest is commit-path CPU (plan log 2026-10-13).

## 2. What transfers from the reference, and what cannot

| reference mechanism | transfers? | why |
|---|---|---|
| variable-length log entries (`src/wal/mod.rs` `LogHeader` + payload) | **yes** | |
| leaf written only on mini-page full / eviction (merge) | **yes** | already built: merge-on-full `3c6fa09`, merge-on-evict |
| log never truncated; recovery = snapshot + log replay (`snapshot.rs`) | **yes, adapted** | our log is compacted instead (section 5) |
| group commit (`append_and_wait` waits for a background flush) | no | single writer, and COMMIT must be durable when it returns (D2) |
| in-place leaf write, one write per merge | no | SQLite readers get snapshot isolation from the WAL: a leaf written in place would show newer rows to an older snapshot. A merge stays WAL frame + backfill, two writes, the same as stock |
| no fsync | no | we keep SQLite's `synchronous` semantics |

The consequence for the per-commit device cost: with one writer and durable commits,
any design writes at least one block per commit (plus an fsync at FULL), and so does
stock. The write win therefore comes from three places:
- coalescing leaf write-back (one leaf write per ~16 records instead of per checkpoint);
- smaller log volume, which means fewer checkpoints, less CPU, and fewer bytes at
  NORMAL, where commits share blocks;
- not writing the dirty set back at every checkpoint.

## 3. Core idea: log pages

A new page type, the **log page** (type byte `0x10`), holds packed variable-length record
entries:

```
log page:  u8 type=0x10 | u8 flags | u16 nEntry | u16 freeOff | u32 nextLogPage
           | u64 firstSeq | entries...
entry:     u64 seq | u8 op (INSERT, DELETE, CLEAR, TOUCH) | u32 leaf | u32 root
           | varint nKey | varint nVal | key | val
```

- The chain head and the allocation extent live in database-header bytes 72–91 (unused
  by SQLite).
- File-format read/write version (bytes 18/19) becomes 3, so stock SQLite refuses the file.
- Integrity check learns the chain.
- `seq` is a global, monotonic log sequence number. It totally orders the log across
  WAL generations, which today's frame numbers cannot do (they reset at every restart).

**A commit appends its entries to the tail log page through the pager**, exactly like
any page modification. The pager writes that page as an ordinary WAL page frame at
commit. Consequences:

1. **The custom WAL format goes away.** No `0xffffffff` record frames, no
   `walBfIngestRecordFrame`, no `walIndexZeroIfBlockStart` / torn-tail workaround, no
   catch-up frame scan. `wal.c` returns to (nearly) stock. Crash safety and atomicity are
   stock's own, which is the strongest durability argument available.
2. **Checkpoint is stock's.** Backfill copies log pages into the db file like any page.
   After a reset, the log is still there, in the db. **Nothing is written back at
   checkpoint.** Checkpoint cost becomes "backfill the distinct pages": the tail log pages
   (~1,000 x ~60 B / 4 KB ≈ 15–40 per 1,000 commits) plus the leaves merges wrote,
   instead of ~850 leaves twice.
3. **Recovery is stock's**, followed by one BF step: read the live log chain through the
   pager and replay (section 4).

## 4. Replay rule (unchanged in spirit)

Leaf L's buffered state at a snapshot = L's entries after its last `CLEAR`, in `seq`
order, over the live log as visible at that snapshot.

This is today's rule (D1). Only the medium changes: log pages instead of record frames,
and `seq` instead of frame numbers. All the D1 reasoning carries over unchanged, in
particular "a page-path write needs its row in base, so a flush (and its `CLEAR`) comes
first".

**New rule, TOUCH completeness.** Every write of a table-leaf page in a BF transaction
logs `TOUCH(L)` unless the transaction already logged `CLEAR(L)`. It is enforced by one
hook where btree makes a leaf page writable.

Today it is only an observation that every changed leaf also gets a `CLEAR` (incremental
invalidation's mutation test, 2026-10-12). Making it a rule lets readers invalidate from
the log alone (section 6).

## 5. Log garbage collection (the reference's snapshot, done incrementally)

The log must not grow forever. The reference's log does.

- An entry is **dead** once its leaf has a later `CLEAR`, or its key has a later op.
  `TOUCH` entries die at the next `CLEAR`, or after every reader has seen them (section 6).
- The writer keeps per-page live counts in memory, rebuilt from the chain at open. A page
  whose entries are all dead is unlinked and freed, in the same transaction as the commit
  that killed it.
- **Compaction:** when chain pages exceed 2x the pages needed for the live entries,
  re-append the live entries at the tail (new `seq`, so order is preserved), then free
  the old pages. This is the incremental analogue of the reference's CPR snapshot. It is
  an ordinary transaction, so a crash mid-compaction is handled by the WAL.
- Bound: live entries ≈ the dirty set ≤ ring budget, so the chain stays ≤ ~2x the dirty bytes.

## 6. Readers (multi-connection) get simpler and work across restarts

A reader remembers the last `seq` it has seen. At a new snapshot it reads the log entries
with `seq` > last-seen from the tail log pages (through its pager, at its snapshot).
Every leaf those entries name (ops, `CLEAR`, `TOUCH`) is dropped and replayed. Nothing
else changed (TOUCH completeness).

This works across WAL resets: `seq` survives them, and so do the log pages. Today a reset
forces a full rebuild in every reader. The frame-scanning catch-up and the touched-page
list in `wal.c` (`7d69956`) are replaced by this.

## 7. Commit-path CPU

Planned removals and changes:
- Entries are serialised straight into the pager's tail log page. Gone: the batch buffer,
  `BatchFinish`'s 4 KB memset, `sqlite3WalBfStage`'s malloc and copy, and the 4 KB record
  frame (the tail page's frame is the commit's only frame).
- No per-op copy into an index on the write path (was ~0.6 µs/commit). The writer keeps a
  per-leaf list of entry locations (page, offset) only for replay, and only for live entries.
- Rollback becomes incremental: drop and replay only the leaves the rolled-back
  transaction touched (known in memory). Today it is a full rebuild.
- Remains structural: the first touch of the leaf's mini-page per write (~1 µs of cache
  misses on a cold mini-page) and the leaf descent. It is the cost of having the record
  cache at all.

## 8. Later stage: delta frames for log pages

Per commit, the tail log page is still a 4 KB WAL frame, of which ~60–200 B are new.

A **delta frame** carries (pgno, offset, bytes) for a log page. It is appended, never
written in place, so it adds no torn-write risk and does not rely on sector atomicity.
Readers and the checkpoint rebuild the page as its last full image plus deltas.

Cost: variable frame sizes break `wal.c`'s fixed stride (`walFrameOffset`), so the
wal-index needs an offset table. It pays off at `synchronous=NORMAL` (commits share
blocks) and through ~20x fewer frames, hence fewer checkpoints. At FULL with one writer,
each commit is one block plus an fsync either way.

Build it only if section 10's measurements say WAL volume still matters after stages
W1–W3. It applies only to log pages, so the effect is attributable to BF.

## 9. Why not persisted mini-pages (v2)

v2 copied each changed leaf's full dirty set into MP pages at every checkpoint. Its cost
grows with the dirty set, not with new records, and it needed its own persist
transaction, tombstones and reset guard.

Log pages persist only what was written, once. They need no checkpoint hook and no reset
guard (the log is in pages, so the WAL may reset freely), and the reference's
log + snapshot structure maps onto them directly.

## 10. Risk register

| risk | mitigation |
|---|---|
| dirty records live longer (until merge, not until checkpoint), so less ring for reads (reads are top priority) | measure the read arms with the write arms; knob `bf_dirty_max` (fraction of the ring) that merges the oldest dirty leaves beyond it (same mechanism as merge-on-evict) |
| open and full rebuild read the whole live log | bounded by ~2x dirty bytes; full rebuilds become rare (section 6) |
| log-page allocation dirties page 1 / the freelist | allocate extents of 64 pages, tracked in the header |
| TOUCH completeness missed somewhere | a debug assert in the page-write hook, plus the mutation test (drop TOUCH must fail the mconn oracle) |
| GC frees a page with a live entry | live counts are recomputed from the chain in debug builds after every GC; the crash oracle runs with aggressive compaction |
| deferred commit (D2, `bf_deferred_commit>1`) | keeps its in-memory batch; appends to the log page only at the Nth commit, as now |

## 11. Stages

Each stage has an ablation switch, counters, and passes the gate, the crash and
power-loss oracles, and the mconn sweep before the next starts.

| stage | content | est. |
|---|---|---|
| **W0** | baseline under direct I/O (WAL + db), NORMAL and FULL: device bytes/commit, µs/commit, page writes per 1,000 commits, stock vs BF, write arms + a 90/10 mix. The yardstick for everything below | 0.5 d |
| **W1** | log pages + chain + seq; commit appends through the pager; recovery replays the chain; checkpoint materialise removed; custom record frames removed from `wal.c`; GC: free all-dead pages (compaction can wait). Crash cases: kill/power loss mid-commit, mid-backfill, after reset, mid-free | 3 d |
| **W2** | readers by seq (section 6), TOUCH rule + assert, incremental rollback; retire the wal.c catch-up and touch list | 1.5 d |
| **W3** | compaction, `bf_dirty_max`, commit-path CPU (section 7), re-profile | 1.5 d |
| **W4** | campaign: write arms + mixed + read arms, under direct I/O on the VPS; decide on section 8 | 0.5 d + run |
| **W5** (conditional) | delta frames | ~3 d |

## 12. Expected outcome (estimates, to be replaced by W4 numbers)

- **Checkpoint:** ~850 leaves x2 → ~15–40 log pages + ~60 merged leaves (≈1,000 commits
  / ~16 records per merge) per 1,000 commits.
- **µs/commit in RAM:** 10.2 → ~6 after W1 (checkpoint ≈ free, one frame per commit,
  no stage copy); W3 aims at ~5. Stock is 3.25. Parity in RAM is not expected: the
  record cache costs a second cold structure per write.
- **Device bytes/commit at NORMAL:**
  - stock ≈ 7.5 KB (4 KB frame + backfill);
  - after W1 ≈ 4.6 KB (4 KB frame + small backfill);
  - with W5 ≈ 1 KB.
- **At FULL:** one block + fsync per commit for both, so latency parity, and fewer
  backfill writes for BF.

## 13. Decisions for the owner

1. Adopt "the record log lives in database pages", replacing record frames and the
   checkpoint write-back?
2. Re-lock: one WAL with stock frame format (W1–W4); log pages and the header fields as
   the BF on-disk format; delta frames only if W4 justifies them.
3. Run W0 now (on this box, or wait for the VPS)?
