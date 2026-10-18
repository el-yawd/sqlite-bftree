# Concurrent BF engine: row-level optimistic transactions, our own log, mini-pages on disk

Status: **ADOPTED** (owner, 2026-10-18: P1-P5 accepted, C3 approved; group commit waits for C4's own log, option D of section 14).  Proposed 2026-10-13. Branch `begin-concurrent-exp`. WAL mode only.
Compatibility is not a constraint (owner). Durability must stay at SQLite's level.
Supersedes `write-path.md` (owner: "I don't like it") and `persisted-mini-pages.md`.

## 0. Why

Upstream BEGIN CONCURRENT (merged in `78a63a25a`) lets transactions run in parallel and
validates at commit **per page**. Two transactions that touch different rows of the same
leaf conflict anyway.

Measured (`bench/concurrent_conflict.c`, rows 500/501 on one leaf):

| | same row | different rows, same leaf |
|---|---|---|
| stock + BEGIN CONCURRENT | 2nd commit rejected (correct) | 2nd commit rejected (**false conflict**) |
| BF + BEGIN CONCURRENT, as merged | **both commit: lost update** | both commit (correct) |

Record buffering removes the false conflict, because a buffered write never modifies the
leaf page. It also hides real conflicts, because page-level validation cannot see record
frames.

This design makes validation row-level and rebuilds the write path around concurrency.
The thesis claim it targets:

> Record buffering turns page-level optimistic concurrency into row-level optimistic
> concurrency, and makes durable group commit effective.

## 1. Principles

- **P1. The log is logical and complete for BF tables.** Every logical change to a BF
  table is a full-value log op: `PUT(table, key, value)`, `DEL(table, key)`,
  `DELRANGE(table)`, `DROP(table)`. That includes changes that also go through the page
  path today (big rows, write-through fallbacks).
- **P2. Ops are idempotent.** Each op sets a key's full value or removes it, so replaying
  any suffix of the log, in log order, over a base that may already contain some of those
  ops gives the right state. This retires CLEAR, and with it every "a page-path write needs
  a flush first" rule: D1's bugs were all about a base newer than a replayed op, which P1
  makes impossible.
- **P3. Uncommitted data never leaves the transaction.** Writes go to a private write set.
  The cache and the log only ever hold committed data, so rollback means discarding the
  set: no rehydrate, no undo.
- **P4. Physical work is never a logical conflict.** Merges, compaction, splits and
  checkpoints run as **system transactions** that change no logical content and never
  abort a user transaction.
- **P5. Durability is never traded.** COMMIT returns only after its log entries are
  durable under the chosen `synchronous` level. Batching across concurrent committers
  (group commit) is the only source of amortization.

## 2. Transactions

### 2.1 Read set (BF tables)
Per table, a transaction records:
- **point reads**: the key, whether found or absent (absent lookups catch phantom inserts);
- **ranges**: the `[lo, hi]` span each scan actually covered, including empty ranges
  (`count(*)` is the whole table).

It is collected in the btree read paths: seek, the merge-scan cursors, and count. If it
outgrows a budget it degrades to "the whole table", which is still correct, just coarser.

### 2.2 Write set
Each transaction has an ordered map `(table, key) → op`, consulted before the cache by its
own reads (read-your-writes). Merge scans merge it as a third source alongside mini-page
and base.

If a transaction outgrows the write-set budget (bulk load), it switches to **exclusive
mode**: it takes the commit lock early and writes directly. It is still logged as ops (P1).

### 2.3 Commit protocol
1. Take the commit lock (the WAL write lock).
2. Read the log entries committed after our snapshot (the catch-up we already have,
   applied to the op stream).
3. **Validate.** Conflict if any committed op's `(table, key)` is in our point set or inside
   one of our ranges. Also apply BEGIN CONCURRENT's page-level rule to the pages that stay
   page-granular (section 2.4). On conflict: release the lock and return
   `SQLITE_BUSY_SNAPSHOT`.
4. Assign LSNs, append the write set and a COMMIT entry to the group-commit buffer, and
   release the lock. The lock is held for validation plus a memcpy, a few µs.
5. Wait until the group flush makes our COMMIT durable (section 3.2).
6. Install the other transactions' ops (from step 2) and then our write set into this
   connection's cache, and advance its LSN.

### 2.4 What stays page-level
Secondary indexes (write-through by v1 scope), WITHOUT ROWID tables, the schema, and page
allocation, which BEGIN CONCURRENT already special-cases.

Pages of BF rowid tables are **left out** of the page read set: their logical content is
validated through ops (P1), and their physical changes are system transactions (P4).

So an UPDATE of an indexed column still conflicts per index page. That is a stated limit,
lifted only if secondary indexes are buffered later.

### 2.5 System transactions
A system transaction is a merge (mini-page full or evicted), a split or rebalance during
a merge, compaction, or the mini-page persist at checkpoint (section 5).

It runs under the commit lock, on the latest state, after the triggering user commit has
returned. It writes page images (section 3.1) and no logical ops. User validation ignores
it (P4).

## 3. Log (our own format, one file `<db>-log`, replacing `-wal`)

### 3.1 Entries
```
entry := u32 len | u8 type | u64 lsn | payload | u32 crc32c(len..payload)
type  := PUT | DEL | DELRANGE | DROP           -- logical (P1), keyed by (tableId, key)
       | PAGE(pgno, image)                     -- physical, for page-level structures + system txns
       | COMMIT(txnId, nOps) | CKPT(lsn, ...)  -- markers
```
- **LSN** is the byte offset in the log, stable and monotonic. The log is truncated from
  the front (section 5), never rewritten.
- **`tableId`** is an incarnation id stored with the table's root, not the root pgno. A
  dropped table's pgno can be reused without its old ops applying to the new table.
- Values of any size: an entry may span blocks.

### 3.2 Writing: durable group commit
- A shared in-process log buffer. A committer copies its entries in, under the commit
  lock (2.3 step 4).
- The first waiter becomes the **leader**: it writes everything pending in one write and
  one fsync (FULL) or none (NORMAL), then publishes the durable LSN.
- **Every flush starts on a fresh block** (block-aligned, direct-I/O friendly). Nothing is
  rewritten in place, so a torn write can damage only the flush in progress, never an
  acknowledged commit. With N committers per flush, the padding is amortized N ways.
- **Cross-process writers** get per-commit flushes (correct, no amortization). Group
  commit is in-process (threads), which is how BEGIN CONCURRENT is used.

### 3.3 Visibility
A commit becomes visible once every LSN before it is durable. The shared-memory header
publishes the **visible LSN**, and a reader's snapshot is a visible LSN.

### 3.4 Page index (replaces the wal-index)
A shared-memory hash maps pgno to its chain of `PAGE` entry LSNs. A reader looks up the
newest LSN ≤ its snapshot, else reads the db file. This is SQLite's wal-index, keyed by
LSN instead of frame number.

## 4. Database file

- SQLite B-tree pages, unchanged as the base representation. Read/write format version
  becomes 3, so stock SQLite refuses the file.
- **Per-page LSN** (later stage): 8 reserved bytes per page (header byte 20) hold the LSN
  of the last merge into a leaf. **It is an optimization, not a correctness requirement
  (P2):** replay skips ops ≤ the leaf's LSN, so it does not re-dirty mini-pages with
  records already merged. After a rebalance, every participating page takes the **minimum**
  of the source pages' LSNs: conservative, and still correct by P2.
- **Mini-page region:** a native page type (`0x10`) packing persisted mini-pages as
  `(tableId, key, value, lsn)` entries; a directory chain; compaction when it is more than
  half dead.
- Header (bytes 72–91): checkpoint LSN, region directory root, next tableId.

## 5. Checkpoint (fuzzy, the reference's CPR snapshot done incrementally)

Goal: advance the **checkpoint LSN C** so the log before C can be truncated.
Requirement: every op ≤ C is in a base page or in the mini-page region (in the db file).

1. Pick C = the visible LSN.
2. A system transaction writes, into the region, every mini-page holding ops ≤ C that are
   neither merged nor already persisted. They go as `PAGE` entries, so they are crash-safe.
3. Copy `PAGE` images ≤ min(C, oldest reader snapshot) into the db file, and fsync.
4. Write C to the header (as a `PAGE` entry of page 1), then truncate the log front up to
   min(C, oldest reader snapshot).

The checkpoint never writes leaves back. That happens only on mini-page full or eviction,
as in the reference.

## 6. Recovery and reopen

1. Read the header's C. Scan the log from C: rebuild the page index from `PAGE` entries
   and collect logical ops whose transaction has a durable `COMMIT`. A transaction's
   entries are contiguous, so a torn tail ends at the last valid CRC.
2. Load the mini-page region into the cache as dirty mini-pages.
3. Apply the ops from C onward, in LSN order (P2; skip those ≤ the leaf's page LSN when present).

No undo phase (P3).

## 7. Readers and caches

- **Snapshot** = the visible LSN at the first read.
- **Cache catch-up** = apply ops with LSN in (cache LSN, snapshot]. That is today's
  incremental invalidation, driven by ops instead of frames. It survives log truncation,
  because LSNs are stable.
- **Caches stay per connection**, like stock's page cache, for a fair comparison. A
  process-wide shared pool is a variant to measure (closer to the reference, and it avoids
  N copies of the same catch-up), not the default.
- The ring holds only committed data (P3). The replay-fit and `ring_grow` rules still apply.

## 8. What goes away, what is new

| goes away | new |
|---|---|
| WAL frame format, record frames, `walIndexZeroIfBlockStart` and the torn-tail workaround | log file format, group commit, page index, visible LSN |
| CLEAR (payload v3), rollback rehydrate, the carry, flush-before-savepoint / flush-before-mutation rules, the dirty-sibling escalation | read/write sets, row-level validation, system transactions |
| checkpoint materialise, close-time materialise | fuzzy checkpoint, mini-page region, table incarnation ids |

## 9. Fairness and attribution

- **Baseline: stock + BEGIN CONCURRENT**, unmodified (the merged branch with
  `SQLITE_OMIT_BF_CACHE`). Its page-level validation and frame WAL are its real design, and
  we do not touch it.
- **Every BF mechanism is switchable:** `BF_ROW_VALIDATION` (off = page-level on record
  frames, i.e. stock's rule), `BF_GROUP_COMMIT` (off = one flush per commit), and the
  mini-page region (off = merge at checkpoint). So each claim is attributable inside one
  campaign.
- **Report:** commits/s, aborts/commit, fsyncs/commit, device bytes/commit; N = 1…16 writer
  threads; uniform vs zipf; rows per leaf; with and without concurrent readers; under
  direct I/O, NORMAL and FULL.

## 10. Correctness tooling

- **Multi-writer serializability oracle** (extends `mconn_oracle.c`): K writer connections
  each run BEGIN CONCURRENT; the model applies successful commits in commit order. Every
  read must match the model at the reader's snapshot, and so must the final state.
  Validated on stock + BEGIN CONCURRENT first.
- **Crash and power-loss oracles** with concurrent writers: kill mid-group-flush,
  mid-checkpoint, mid-truncation.
- **Mutation tests:** drop range tracking, drop absent-key tracking, drop P1 logging of a
  page-path write. Each must fail the oracle.
- The existing gate, differential and single-writer crash oracles stay green at every stage.

## 11. Stages

Each stage is usable and measurable on its own.

| stage | content | proves | est. |
|---|---|---|---|
| **C1** | row-level validation on the CURRENT format: read sets; validate record frames since the snapshot; BF-table pages out of the page read set, except leaf pages changed by non-merge page writes (pre-P1); multi-writer oracle | false conflicts gone, soundly | 1 wk |
| **C2** | private write sets (P3) + durable group commit over the current WAL (batched record frames per flush) | concurrency write win; rollback rehydrate retired | 1.5 wk |
| **C3** | P1 complete logical log + tableIds: page-path writes also log ops; CLEAR retired; replay by P2 | simpler recovery; splits stop mattering | 1.5 wk |
| **C4** | own log format + page index + mini-page region + fuzzy checkpoint (sections 3–6) | checkpoint write-back and frame waste gone | 3 wk |
| C5 | per-page LSN, shared-pool variant, campaign | optimization + final numbers | 1 wk |

C1–C2 already carry the thesis claim. C3–C5 make the engine reference-faithful and remove
the write-path costs measured on 2026-10-13 (checkpoint 3.4 µs/commit, 4 KB frame per
commit).

## 12. Risks

| risk | mitigation |
|---|---|
| phantom protection misses a scan path | every cursor entry point records its range; the mutation test drops one at a time |
| write-set reads double the merge-scan complexity (3 sources) | reuse `bfMergePick`; the differential oracle under BEGIN CONCURRENT with read-your-writes workloads |
| catch-up cost per commit with many writers (N connections replay each other's ops) | measured; the shared-pool variant is the answer if it dominates |
| group-commit leader stalls (fsync latency) | standard leader/follower with a timeout; FULL vs NORMAL measured separately |
| own log format bugs (C4) | ESBMC + libFuzzer on the entry codec and recovery scan, as for `bf_wal.c` |
| scope (≈8 weeks) | C1–C2 alone are a complete result; C3+ only after they measure well |

## 13. Decisions for the owner

1. Adopt P1–P5 as the new locked principles, replacing "one SQLite WAL, page-sized frames",
   CLEAR, and the single-writer lock?
2. Is group commit in-process only (threads) acceptable? Cross-process writers stay correct
   but unamortized.
3. Caches per connection (fair) as the default, shared pool as a measured variant?
4. Start C1?

## 14. C2 status and the group-commit decision (2026-10-17)

Done on the current format: C1 (row-level validation; false conflicts ~1,500 -> ~110 per
mwriter run at 4 writers), C2a (rollback replays only what the transaction touched -- P3 for
per-connection caches), a threaded oracle (`bench/mt_oracle.c`) and the three concurrency bugs
it found (checkpoint backfill cap, catch-up at read-mark 0, page renumbering at commit).

Not done: **durable group commit (section 3.2) over SQLite's WAL**, because it needs a decision
the text above leaves open.  Under `synchronous=FULL` a WAL commit writes its frames, fsyncs,
then publishes the wal-index header -- all under the WAL write lock.  Amortising the fsync across
committers means one of:

| option | visibility | durability of an acknowledged COMMIT | cost |
|---|---|---|---|
| A. fsync outside the lock: publish, release, then fsync (followers whose frames a finished fsync already covers skip theirs) | **before** durability: a reader can see a commit that a crash then loses (the committer never got OK) | kept | small: `walFrames` split; no format change |
| B. pending header: publish to an in-process "writers' header" at once, to the shm header (readers) only after the covering fsync | after durability (section 3.3) | kept | an in-process per-file structure shared by connections; writers in OTHER processes must be excluded (or they read the stale shm header and overwrite frames) -- section 13.2's "in-process only" |
| C. leader commits for followers (one lock, one write, one fsync, one publish) | after durability | kept | the leader must validate and write other connections' transactions: their pagers, read sets and BF caches, across threads -- the most invasive |
| D. no group commit at C2; take it in C4's own log, where section 3.2 designs it from scratch | after durability | kept | none now |

SQLite's own `synchronous=NORMAL` already behaves like A minus the fsync, and D2 settled the
durable default mode as "records reach the OS before COMMIT returns" (no fsync, like the
reference).  So group commit only changes `synchronous=FULL` numbers.  Recommendation: **D** for
the thesis path (C3 next), and **A** behind a PRAGMA only if FULL-mode concurrent throughput is
needed before C4, reported with its visibility caveat.  Owner decision.

## 15. C3 implementation plan (2026-10-18)

C3 makes the record log logical while it still rides SQLite's WAL (C4 replaces the WAL).

**Format v5.**  Ops INSERT / DELETE keep their fields; the leaf page number becomes a HINT,
never trusted for correctness.  CLEAR(leaf) is replaced by
`MERGED(root, lo, hi, snap, start)`: "every op on a key in [lo, hi] of table `root` that
precedes this marker is in base pages -- except ops in frames (snap, start)".  `lo`/`hi` are the
smallest and largest key the merged mini-page held; `snap` is the merging transaction's
snapshot frame and `start` the first frame of its own commit, so ops other transactions
committed after its snapshot (BEGIN CONCURRENT) are NOT covered.  For a single writer
snap = start-1 and the exception is empty.

**Index.**  Keyed by (root, rowid), not by leaf: per key its ops in log order, per table its
markers.  A key's replay state is its LAST op unless a marker covers it -- every op is a full
value (P2), so earlier ops never matter.

**Replay places by key.**  Each record goes into the mini-page of the leaf that holds its key
NOW (a descent through interior pages, which stay resident outside the budget).  So a split or
merge that moved a key cannot strand its record (the D1 / C1 hazard).  Incremental replay
(catch-up, rebase, rollback) works on KEYS: those of new record ops, plus those of the dirty
records in every leaf a page frame changed (whose mini-pages are dropped first).

**C1 rules retired:** "another op on a leaf we wrote" (placement is by key now), "another op on a
leaf we merged" (the marker's snap), "another CLEAR on a leaf we read" -- replaced by: a foreign
MERGED conflicts iff our read set intersects [lo, hi] (a merge inside a transaction can apply
writes that never become ops -- the P1 gap C4 closes by logging them).

**Not in C3:** logging page-path writes as ops (the flush-before-page-write rules still order
them), capturing merged in-transaction writes as ops, retiring the live-cache flush rules.

**Implementation notes (built 2026-10-18).**  Two rules the plan above did not state, each found
by a BEGIN CONCURRENT oracle failure:
- A marker's `start` is the SHARED log head + 1 (`sqlite3WalBfHeadFrame`), read while the commit
  holds the write lock.  The connection's own header is still its snapshot when the markers are
  logged (btreeFixUnlocked upgrades it later in CommitPhaseOne), and a `start` taken from it
  made the marker cover rows other transactions committed in between (mwriter_oracle seed 2).
- The commit scan's placement descent (`sqlite3BfLeafForKeyCached`) reads NO page: it runs after
  upstream's walLockForCommit has dropped the cached pages others changed and before the
  snapshot upgrade, so a page read then comes from the old snapshot and outlives the upgrade --
  a stale leaf that later served an old row (seed 3).  An uncached path falls back to the op's
  hint.  Replay's own descent (`sqlite3BfLeafForKey`) reads pages normally, at a consistent
  snapshot, and ignores BF's known-leaf bitmap (a page can have become interior).
