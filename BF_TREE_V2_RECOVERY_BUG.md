# Crash recovery writes a structurally malformed B-tree

**Status:** OPEN. Severity: **top**. Found 2026-09-21, written up 2026-09-22.
**Repro:** `sh bench/recover_repro.sh` — fails all three scenarios.
**Blocks:** every durability claim in the thesis. Also blocks §7.2 (multi-writer), because
concurrency over an unsound recovery path is not worth building.

---

## 1. The problem in one paragraph

Kill a connection after it has committed a transaction whose rows are still buffered as BF
record frames in the WAL. Reopen. Every committed row is present and readable — but the
B-tree that recovery leaves on disk is **structurally invalid**, and any subsequent write to
that database **loses committed rows**. Stock SQLite under the identical crash pattern is
clean, and the same workload closed cleanly is clean. So this is ours, and it is specific to
recovering record frames.

```
PRAGMA integrity_check;
*** in database main ***
Tree 2 page 2 cell 24: Child page depth differs
```

---

## 2. Minimal reproducer

`bench/recover_repro.sh` automates this. By hand:

```sh
DB=/tmp/rc.db; rm -f $DB $DB-wal $DB-shm
{ echo "PRAGMA journal_mode=wal;"
  echo "PRAGMA wal_autocheckpoint=0;"        # keep record frames in the WAL
  echo "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);"
  echo "BEGIN;"
  i=1; while [ $i -le 2000 ]; do
    echo "INSERT INTO t VALUES($i,'v$i-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx');"
    i=$((i+1)); done
  echo "COMMIT;"
  echo "SELECT 'loaded';"
  # keep the process alive so nothing checkpoints on close
  i=0; while [ $i -lt 2000000 ]; do echo "SELECT 1;"; i=$((i+1)); done
} > /tmp/rc.sql

./build/sqlite3_buf $DB < /tmp/rc.sql > /tmp/rc.out 2>&1 &
BG=$!; while ! grep -q loaded /tmp/rc.out 2>/dev/null; do :; done
kill -9 $BG                                   # crash, WAL intact

./build/sqlite3_buf $DB "PRAGMA integrity_check;"   # -> Child page depth differs
./build/sqlite3_buf $DB "SELECT count(*) FROM t;"   # -> 2000, all rows there
```

A clean close must be prevented: SQLite checkpoints on last-connection close, which
materialises the record frames and hides the defect.

---

## 3. What is established

Each line below is a measurement, not an inference.

| # | Observation |
|---|---|
| 1 | After recovery, **all committed rows are readable**: 2000/2000, and `.dump` is stable across repeats. |
| 2 | `integrity_check` reports `Tree 2 page 2 cell NN: Child page depth differs` — the root's children are at differing depths. |
| 3 | The **base file alone** (crash image with `-wal` removed) is pristine: `integrity_check` = `ok`, and it does not even contain table `t` — `CREATE TABLE` was still in the WAL. **The crash left the database file undamaged.** |
| 4 | The defect is present **immediately after recovery, before any checkpoint**. The checkpoint is not the cause. |
| 5 | It **persists** through materialisation + `wal_checkpoint(TRUNCATE)` + a fresh reopen. It is not a transient view. |
| 6 | On that materialised file (empty WAL), **stock SQLite itself reports the corruption**. The damage is written to disk; it is not a BF read-path artifact. |
| 7 | **Subsequent writes lose committed rows.** 2000 rows + 500 plain independent inserts → `count(*)` = **2344**, expected 2500 (156 lost). With `INSERT ... SELECT` from the same table → **1577** (923 lost). |
| 8 | After those writes, `integrity_check` additionally reports a **leaked page**: `Page 15: never used`. |
| 9 | **Stock SQLite under the identical crash pattern is clean** in all three `recover_repro.sh` scenarios. |
| 10 | The same workload with a **clean close is clean**, in-session and on reopen. The crash is necessary. |
| 11 | A `SQLITE_DEBUG` build fires **no assert** — neither during recovery nor during the lossy write. No internal invariant is violated on the way; the tree is simply built wrong. |

### The misleading control, recorded so nobody repeats it

Reading the crash image with `PRAGMA bf_cache=off` reports `ok`. **This does not mean the data
is fine.** That reader cannot interpret record frames, so it sees a smaller, self-consistent
tree: its `.dump` is 1972 lines against 2004 with the cache on, missing rows 1969–2000 — the
32 rows that live only in record frames. The "ok" is a verdict on a different, smaller
database. I briefly retracted this whole finding on the strength of that reading; item 6 is
what settles it.

---

## 4. Ruled out, with the evidence

Do not re-investigate these without new information.

* **The documented leaf→root gap is NOT the defect.** `sqlite3BfCacheReplayWal`'s KNOWN GAP
  comment says a replay-created mini-page has no `rootPgno` (the WAL stores only the leaf
  pgno), and `bfFlushAllCallback` skips `rootPgno<=1`. `recover_repro.sh` tests all three
  checkpoint orders — descend-first, checkpoint-first, checkpoint-in-a-separate-process — and
  **no rows are lost to it in any of them**. This hazard has been carried as "future work"
  since Phase 2 and it is not what is breaking.
* **Not a self-referencing-statement bug.** A fresh database runs the same
  `INSERT ... SELECT ... FROM t` correctly: 2500 rows, `integrity_check` = `ok`.
* **Not `bfApplyOneRecord` placing rows by a stale leaf.** It applies each record with
  `sqlite3BtreeInsert(pCur, &payload, 0, 0)`; with `seekResult == 0` SQLite positions the
  cursor by key itself, so the logged leaf pgno is not used for placement.
* **Not a reader-side artifact.** Item 6: stock agrees the materialised file is corrupt.
* **Not the checkpoint.** Item 4: it precedes any checkpoint.

---

## 5. Hypotheses, ranked

Each names the mechanism, why it fits, why it might not, and the experiment that settles it.

### H1 — Record ops cannot be ordered against page images (shadowing)

`BfWalRec` (`src/bf_wal.h:54-62`) carries `pgno`, `op`, key and value — **no frame number**.
So `sqlite3BfCacheReplayWal` → `bfReplayOnePage` (`src/bf_cache.c`) has no way to tell whether
a record op was superseded by a later page-image frame for the same page.

Ordering makes this reachable: in `walFrames` (`src/wal.c`) the staged record frames are
emitted **before** the page-image frames of the same commit. Mid-transaction flushes
(`sqlite3BfBtreeFlushTableForMutation`, triggered whenever a buffered insert is refused)
materialise rows into base pages, which are then written as page images at commit. Any record
for an already-materialised row that is still logged is therefore **older** than the page image
that contains it, and replay re-applies it regardless.

* **Fits:** explains why only a crash triggers it (a clean close checkpoints, and the live
  cache is authoritative until then); matches the already-documented failure mode in memory as
  `phase2-flag-on-shadowing` ("stale record replayed over newer base").
* **Against:** re-inserting an existing rowid is an *overwrite*, which should not unbalance a
  tree on its own.
* **Experiment:** add the frame number to the replay index and skip ops superseded by a later
  page-image frame for the same pgno; or, cheaper first, instrument replay to count how many
  recovered ops target a key that already exists in base. If that count is zero, H1 is dead.

### H2 — Records replayed against pages that are no longer leaves of that table

The WAL stores only the leaf pgno. Between the record being logged and recovery finishing,
later page images in the same WAL may have **freed or repurposed** that page — as an interior
page, an overflow page, or a freelist page. Replay attaches records to the pgno anyway;
`bfTagLeafRoot` later stamps it with whatever root a descent happens to pass through; and the
records are then materialised as if that page meant what it meant at log time.

* **Fits:** explains structural damage *and* the leaked page in item 8 far better than H1 does.
* **Against:** materialisation positions by key (§4), so misplacement would have to come from
  the grouping/mapping rather than from cell placement.
* **Experiment:** at replay, check whether each pgno is currently a leaf of the tagged root
  before materialising, and count rejects. A non-zero count confirms it.

### H3 — `nTruncate` / header page-count disagreeing with the real page set

Two runs producing **byte-identical WALs** left base files differing at **byte 28** — the
database header's page-count field. If recovery restores a page count inconsistent with the
pages actually present, the allocator can hand out pages that are still referenced, and free
pages that are not.

* **Fits:** this is the cleanest explanation for *both* "child page depth differs" *and* "page
  never used", and for rows vanishing on later writes rather than immediately.
* **Caveat:** that byte-28 observation was taken after probe runs that had themselves
  checkpointed on close, so it is **not clean evidence** — it needs re-measuring on pristine
  copies before being trusted.
* **Experiment:** after recovery, compare the header page count against the true page count and
  against `max(pgno)` in the WAL; re-run the byte-28 comparison on untouched copies.

### H4 — Materialisation racing the statement that triggered it

`sqlite3BfCacheReplayWal` sets `bDirtyInserts = 1`. The first descent tags roots, and the first
*mutation* then triggers `FlushTableForMutation`, which materialises recovered records through
a bypass cursor **while the triggering statement is itself mid-flight**. Cursor invalidation
and re-seek around that boundary is exactly where the Phase-1 scan bug lived.

* **Fits:** damage appears and worsens on write; rows are lost by later inserts (item 7).
* **Against:** the defect is already visible before any write (item 4), so at best H4 makes an
  existing defect worse.
* **Experiment:** materialise everything immediately after recovery inside a dedicated
  transaction, with no other statement in flight, then check integrity.

### H5 — Double materialisation plus map re-pointing

Rows may exist both in base (materialised pre-crash by eviction or flush) and in the WAL as
records. Replay adds them to the cache as dirty `BFOP_INSERT`s; the merge path serves them; and
materialisation applies them again. Combined with the upgrade/compaction paths that re-point
`pEntry->pPage`, the mapping can end up describing a page set that no longer matches the tree.

* **Related:** the `pEvictProtect` defect fixed in `ea536fc` was the same family — a map entry
  left describing something the cache no longer held.
* **Experiment:** count recovered ops whose key is already present in base (shared with H1).

---

## 6. Code map

| What | Where |
|---|---|
| replay entry point | `src/bf_cache.c` — `sqlite3BfCacheReplayWal`, `bfReplayOnePage` |
| the documented leaf→root gap | `src/bf_cache.c`, comment above `sqlite3BfCacheReplayWal` |
| record struct, **no frame number** | `src/bf_wal.h:54-62` (`BfWalRec`) |
| record frames emitted before page images | `src/wal.c`, the `nBf` loop in `walFrames` |
| recovery's frame walk | `src/wal.c`, `walIndexRecover` — `WAL_BF_RECORD_PGNO` arm |
| applying one record to base | `src/bf_btree.c` — `bfApplyOneRecord` |
| per-leaf flush | `src/bf_btree.c` — `bfFlushOneMiniPage`, `bfFlushTableDirty` |
| root tagging | `src/bf_btree.c` — `bfTagLeafRoot` |
| checkpoint materialisation | `src/btree.c` — `bfCheckpointMaterialize` |

---

## 7. Next steps, in order

1. **Run the two counting experiments** (H1/H5 share one, H2 has its own). Both are
   instrumentation only, no behaviour change, and between them they should eliminate at least
   two hypotheses in a single run.
2. **Re-measure the byte-28 observation on pristine copies** (H3). Cheap, and if it holds it is
   probably the whole answer.
3. Only then choose the fix. If H1/H2 survive, the fix is likely D1's *direct replay* option —
   recovered records applied by key from the owning table root rather than rehydrated into
   mini-pages keyed by a logged leaf pgno — which probably means **adding the root pgno to the
   record format**. That is permitted: §4.2 states this fork has no on-disk compatibility
   requirement. It would also close the leaf→root gap permanently, and it answers R10, which
   was deferred pending exactly this evidence.
4. Re-verify on `main`. This was found on `main`; the `wal-commit-frame-merge` branch touches
   the commit path, so the reproducer must be re-run there before that branch merges.

---

## 8. Instrumentation already in the tree

`src/bf_btree.c` carries a `SQLITE_BF_RECOVERY_TRACE` block (off by default, preserved in
`26a72a1`). Build with `-DSQLITE_BF_RECOVERY_TRACE` and it prints:

```
BFFLUSH owner= root= records= stale= nPage=      per bfFlushOneMiniPage
BFAPPLY owner= rowid= exists= leaf= depth= nPage= rc=   per applied record
```

`exists` comes from a `sqlite3BtreeTableMoveto` probe before the insert, so it answers **step 1
of §7 directly**: how many recovered ops target a key that is already in base. `leaf` and
`nPage` speak to H2 and H3. Run this before writing any new instrumentation.

**Lost companion, and my fault.** `bench/recover_trace_probe.sh` sat untracked beside it and I
deleted it while tidying the branch, without reading it first — a direct violation of this
project's rule to preserve uncommitted work. It is unrecoverable and I cannot reconstruct it,
because I never saw its contents. Only the driver script is gone; the instrumentation it drove
survives above.

## 9. Note on the oracles

The single-table differential oracles never produced this, and could not: they close cleanly,
so the record frames are always checkpointed before anything checks. `recover_repro.sh` exists
because of that gap, and it gates on row count **and** `integrity_check` — an earlier version
of it checked only the count, printed `ALL CLEAN`, and displayed the corruption in passing.
A test that reports success while showing corruption is worse than no test.
