/*
** 2024 January 01
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This header file defines the Bf-Tree cache subsystem interface.
**
** The Bf-Tree cache replaces SQLite's standard page cache with a
** variable-length mini-page architecture for improved cache efficiency
** and reduced write amplification. Based on the Bf-Tree paper (VLDB 2024).
**
** Key concepts:
**   - Mini-pages: Variable-length (64-4096 bytes) cached subsets of disk pages
**   - Circular buffer: Ring buffer for mini-page allocation with FIFO eviction
**   - Copy-on-access: Hot data copied to tail to prevent eviction
**   - Operation types: INSERT, DELETE, CACHE, PHANTOM for record tracking
*/

#ifndef _BF_CACHE_H_
#define _BF_CACHE_H_

/* Forward declarations */
typedef struct BfCache BfCache;
typedef struct BfCircularBuffer BfCircularBuffer;
typedef struct BfMiniPage BfMiniPage;
typedef struct BfMapEntry BfMapEntry;
typedef struct BfAllocMeta BfAllocMeta;
typedef struct BfKVMeta BfKVMeta;
typedef struct BfFreeList BfFreeList;

/*
** Size class configuration for mini-pages.
** Mini-pages start at BF_MIN_MINI_PAGE and double until BF_MAX_MINI_PAGE.
*/
#define BF_MIN_MINI_PAGE      64
#define BF_MAX_MINI_PAGE      4096
#define BF_SIZE_CLASS_COUNT   7

/* Size-class derivation (B1), matching the reference.
**
** ../bf-tree does not use a power-of-two ladder.  It DERIVES the classes from
** the record size (BfTree::create_mem_page_size_classes, tree.rs:222-250):
**
**     class(k) = 2^k * (minRecord + sizeof(KVMeta)) + sizeof(LeafNode)
**
** rounded up to a cache line, ascending, stopping at the largest mini-page,
** with the full leaf page size as the final class.  The ladder is therefore
** "room for 1, 2, 4, 8 ... records" rather than "64, 128, 256 ... bytes", which
** is a better fit for the thing actually being stored.
**
** For our geometry (BF_MIN_RECORD 64 + 8 B BfKVMeta, 24 B BfMiniPage header,
** 64 B lines) that gives 128, 192, 320, 640, 1216, 2368, 4096 -- against the
** old 64, 128, 256, 512, 1024, 2048, 4096.
**
** Why it should matter here: a lone cached record (1.20-1.23 per mini-page
** under scrambled zipf) needs 24 + 8 + 8 + 100 = 140 B, which takes the 256 B
** class on the old ladder and the 192 B class on this one -- 25% less ring per
** record, measured at 250 B/record before the change.  It also raises the
** FIRST class from 64 to 128: a fresh mini-page is allocated at aSizeClass[0]
** (bf_cache.c), and 64 B leaves only 32 B for a record after the header, so
** every real row upgraded on its first insert.
**
** CAVEAT, and the reason BF_MIN_RECORD must eventually be configurable as it is
** in the reference (cb_min_record_size, a Config field their benchmark sets per
** workload): the ladder is only well matched to records near BF_MIN_RECORD.
** For the paper's own workload -- key 16 B, value = the key, so 24 + 8 + 16 +
** 16 = 64 B for one record -- the OLD ladder's 64 B first class was an exact
** fit and this one rounds to 128, i.e. 2x worse.  For our 100 B values it is
** 25% better.  Hard-coding 64 therefore tunes for OUR benchmark and detunes the
** replication, which is backwards for a change whose whole justification is
** fidelity.
**
** RESOLVED: this is PRAGMA bf_min_record, defaulting to the reference's 64.
** configs/b1.json measured the consequence of getting it wrong -- a 200-byte
** value loses 18.6% of the records it could cache at N=64, while a 100-byte
** value gains 27.7%.  Set it to the workload's record size. */
/* Ladder base for the derived size classes (PRAGMA bf_min_record).
**
** 64 is OURS, chosen by measurement in B1 -- it is NOT the reference's default,
** which an earlier version of this comment claimed.  `../bf-tree`
** config.rs:27 has DEFAULT_MIN_RECORD_SIZE = 4, and nothing in its benchmark/
** or dev/ trees overrides it, so the reference runs at 4.  Its ladder is
** therefore far finer-grained and starts far smaller than ours. */
#define BF_DEFAULT_MIN_RECORD 64
#define BF_CACHE_LINE         64

/*
** Default configuration values.
**
** BF_DEFAULT_BUFFER_SIZE: the circular buffer backing the mini-page pool.
** Must be a power of 2 and >= BF_MAX_MINI_PAGE + sizeof(BfAllocMeta).
** 8 MB gives ~116k 64-byte mini-page slots, adequate for a benchmark
** database with thousands of hot records.
*/
#ifndef BF_DEFAULT_BUFFER_SIZE
# define BF_DEFAULT_BUFFER_SIZE   (8*1024*1024)   /* 8 MB circular buffer */
#endif
#define BF_DEFAULT_COPY_ON_ACCESS 0.1             /* 10% copy-on-access region */
/* Percent of read misses that copy the record into the cache.
**
** 5 was far too low.  The record cache's whole advantage on skewed point reads
** is holding the hot set, and at 5% it never gets there: a 60M-row zipf-0.99
** read workload cached 48k records and ran at PARITY with stock.  At 30 -- the
** rate ../bf-tree uses for its storage benchmark (benchmark/bench_bftree.toml;
** it uses 100 for the in-memory one) -- the same workload caches 256k records,
** reads 21% fewer bytes, and beats stock by 1.15x.  Measured 2026-09-15,
** 120 s warmup so the ring actually saturates:
**
**    rate   ops/s    cached   rec hit%   rss
**      5    10,841    48,321    48.3%   164 MiB
**     30    12,476   255,502    55.0%   238 MiB
**    100    13,169   509,654    52.4%   305 MiB
**
** 100 is marginally faster still but spends 67 MiB more of the budget on
** records for it, and the hit rate does not improve -- at 100 every one-off key
** is promoted too.  30 keeps most of the win.  PRAGMA bf_promotion_rate tunes
** it per connection. */
#define BF_DEFAULT_PROMOTION_RATE 30              /* 30% read promotion rate */
/* Upper bound on PRAGMA bf_group_commit: how many transactions may share one
** open record batch before it is forced out to the WAL.  A crash loses at most
** this many committed transactions, so keep it small enough to stay a
** defensible durability trade. */
#define BF_MAX_GROUP_COMMIT       1024
/* Tables tracked for "max buffered rowid" (see BfCache.aMaxRoot). */
#define BF_MAXROWID_SLOTS         8

/*
** Operation types for records in a mini-page.
** These determine whether a record is dirty and its logical state.
*/
#define BFOP_INSERT   0     /* Dirty: newly inserted record */
#define BFOP_DELETE   1     /* Dirty: tombstone (deletion marker) */
#define BFOP_CACHE    2     /* Clean: cached record from base page */
#define BFOP_PHANTOM  3     /* Clean: cached deletion (negative lookup) */

/*
** State values for allocation metadata in circular buffer.
** Tracks the lifecycle of each allocation.
*/
#define BF_STATE_NOT_READY      0   /* Just allocated, not yet usable */
#define BF_STATE_READY          1   /* In use, can be accessed */
#define BF_STATE_BEGIN_TOMBSTONE 2  /* Being deallocated (x-lock acquired) */
#define BF_STATE_TOMBSTONE      3   /* Deallocated, can be reused */
#define BF_STATE_FREELISTED     4   /* In free list for reuse */
#define BF_STATE_EVICTED        5   /* Evicted, head can advance past */

/*
** Page location types.
** Tracks where a page's data resides.
*/
typedef enum BfPageLocation {
  BF_LOC_NULL = 0,    /* Page not in cache */
  BF_LOC_BASE,        /* Only on disk at offset */
  BF_LOC_MINI,        /* Mini-page delta chain in circular buffer */
  BF_LOC_FULL         /* Full page mirror in circular buffer */
} BfPageLocation;

/*
** Allocation metadata structure.
** Stored immediately before each allocation in circular buffer.
** 8 bytes total, aligned to 8-byte boundary.
*/
struct BfAllocMeta {
  u32 size;           /* Allocated size (not including metadata) */
  u8 state;           /* BF_STATE_* values */
  u8 reserved[3];     /* Alignment padding */
};

/*
** Free list for recycling deallocated memory.
** Maintains linked lists per size class.
*/
struct BfFreeList {
  void *apHead[BF_SIZE_CLASS_COUNT];    /* Head of each size class list */
  u32 aSizeClass[BF_SIZE_CLASS_COUNT];  /* Size for each class */
  sqlite3_mutex *mutex;                  /* Protects free list access */
};

/*
** Circular buffer for variable-length mini-pages.
** Inspired by FASTER's hybrid log design.
*/
struct BfCircularBuffer {
  u8 *pBuffer;              /* Raw buffer memory (power-of-2 size) */
  u64 capacity;             /* Total buffer size in bytes */
  u64 headAddr;             /* Logical address of oldest entry */
  u64 tailAddr;             /* Logical address for next allocation */
  u64 evictingAddr;         /* Address being evicted (for async eviction) */
  sqlite3_mutex *mutex;     /* Protects head/tail manipulation */

  BfFreeList freeList;      /* Free list for memory recycling */

  /* Configuration */
  double copyOnAccessRatio; /* Threshold ratio for copy-on-access (0.1) */
  u64 copyOnAccessThreshold;/* Computed threshold in bytes */

  /* Statistics */
  u64 nAllocs;              /* Total allocations */
  u64 nEvictions;           /* Total evictions */
  u64 nFreeListHits;        /* Allocations from free list */
};

/*
** Record metadata within a mini-page.
** 8 bytes per record, stored in array after mini-page header.
*/
struct BfKVMeta {
  u16 offset;               /* Offset to key/value data from page end */
  u16 keyLenAndOp;          /* Bits 0-13: key length, 14-15: operation type */
  u16 valueLenAndRef;       /* Bits 0-13: value length, 14: WAL-logged flag,
                            ** 15: referenced flag */
  u8 preview[2];            /* First 2 bytes of key (for fast comparison) */
};

/* Macros for BfKVMeta field access.
**
** The "logged" flag (bit 14 of valueLenAndRef) marks a dirty record
** (BFOP_INSERT/BFOP_DELETE) whose op has already been written to the WAL as a
** physiological record frame in the CURRENT WAL generation (Phase 2).  It is
** orthogonal to the 2-bit op: a logged record is still DIRTY (not yet in the
** base B-tree) — it must still be flushed to base at checkpoint/eviction — but
** the commit-time gather skips it so its op is logged exactly once.  The value
** length is bounded by BF_MAX_MINI_PAGE (4096) << 0x3FFF, so narrowing it from
** 15 to 14 bits is safe and frees bit 14 for this flag. */
#define BF_KV_KEY_LEN(m)     ((m)->keyLenAndOp & 0x3FFF)
#define BF_KV_OP_TYPE(m)     (((m)->keyLenAndOp >> 14) & 0x03)
#define BF_KV_VALUE_LEN(m)   ((m)->valueLenAndRef & 0x3FFF)
#define BF_KV_IS_LOGGED(m)   (((m)->valueLenAndRef >> 14) & 0x01)
#define BF_KV_IS_REF(m)      (((m)->valueLenAndRef >> 15) & 0x01)

#define BF_KV_SET_KEY_LEN(m, len)   ((m)->keyLenAndOp = ((m)->keyLenAndOp & 0xC000) | ((len) & 0x3FFF))
#define BF_KV_SET_OP_TYPE(m, op)    ((m)->keyLenAndOp = ((m)->keyLenAndOp & 0x3FFF) | (((op) & 0x03) << 14))
#define BF_KV_SET_VALUE_LEN(m, len) ((m)->valueLenAndRef = ((m)->valueLenAndRef & 0xC000) | ((len) & 0x3FFF))
#define BF_KV_SET_LOGGED(m, lg)     ((m)->valueLenAndRef = ((m)->valueLenAndRef & 0xBFFF) | (((lg) & 0x01) << 14))
#define BF_KV_SET_REF(m, ref)       ((m)->valueLenAndRef = ((m)->valueLenAndRef & 0x7FFF) | (((ref) & 0x01) << 15))

/*
** Mini-page header structure.
** Variable-length pages in circular buffer (64-4096 bytes).
**
** Layout:
**   +------------------+
**   | BfMiniPage (24B) |
**   +------------------+
**   | BfKVMeta array   | (grows forward)
**   | ...              |
**   +------------------+
**   | (free space)     |
**   +------------------+
**   | key/value data   | (grows backward from end)
**   +------------------+
*/
/* BfMiniPage.flags bits.
**
** BF_MINI_F_DIRTY is a CONSERVATIVE hint: it is set whenever a dirty record
** (BFOP_INSERT/BFOP_DELETE) is written and cleared only by
** sqlite3BfMiniPageMarkClean, which converts every dirty record to a clean one.
** So flag clear ⇒ definitely no dirty records; flag set ⇒ scan to be sure.  The
** over-approximation costs at most a wasted scan, never a lost write.
**
** It exists because sqlite3BfMiniPageIsDirty was 82% of the profile on an
** append workload: the pre-mutation flush asks every mini-page in the map
** whether it is dirty, on every mutation, and the answer required a full record
** scan -- worst for CLEAN pages, which scan to the end before returning 0. */
#define BF_MINI_F_DIRTY  0x0001
/* Set when a balance changed this leaf's key range while a flush replay was
** iterating the mini-page, so the stale CLEAN records could not be dropped
** right then (rewriting the page mid-iteration loses dirty records).  The
** flush drops them when it finishes. */
#define BF_MINI_F_STALE  0x0002
/* BF_MINI_F_UNLOGGED is the same kind of conservative hint as BF_MINI_F_DIRTY,
** for the other question a commit asks: does this mini-page hold a dirty record
** that is not in the WAL yet?  Set wherever a fresh mutation lands (which is
** exactly where BF_KV_SET_LOGGED(...,0) is written), cleared by the commit-time
** gather once it has walked the whole page.  Flag clear => nothing to log.
**
** It exists so a commit can visit the pages THIS transaction touched instead of
** every dirty mini-page in the cache.  The dirty list cannot answer that: a
** mini-page stays dirty until it is flushed to its base page, so with one row
** per transaction the commit-time walk grew with the whole accumulated dirty
** set -- 24.9% of cycles on an insert workload, and O(N^2) over N commits. */
#define BF_MINI_F_UNLOGGED 0x0004

struct BfMiniPage {
  u16 nodeSize;             /* Total size of this mini-page */
  u16 metaCount;            /* Number of records (including fence keys) */
  u16 freeSpace;            /* Free space between meta array and data */
  u16 flags;                /* BF_MINI_F_* bits.  Was `prefixLen`, a field left
                            ** over from an unimplemented prefix-compression
                            ** scheme: assigned and copied, never read.  Reusing
                            ** it keeps the header at 24 bytes -- growing it
                            ** would cost 8 bytes of padding on EVERY mini-page,
                            ** i.e. 12% of the 64-byte size class. */
  i64 baseDiskOffset;       /* Disk offset of base page (-1 if none) */
  /* Back-pointers into the mapping table.  ownerPgno is the page number this
  ** mini-page is mapped under (the table root today; the hot leaf after
  ** Phase 1 re-keying) — it lets the FIFO eviction sweep find the owning map
  ** entry with a direct sqlite3BfMapLookup instead of a linear scan.  rootPgno
  ** is the table's root page, used to group "flush all dirty leaves of table
  ** X" and ClearCache.  Both occupy the 8 bytes formerly held by the unused
  ** recovery `lsn` field (header size unchanged at 24 bytes). */
  u32 ownerPgno;            /* Map key this mini-page is reachable under */
  u32 rootPgno;             /* Root page of the owning table */
};

/* Compute pointer to BfKVMeta array (immediately after header) */
#define BF_MINI_PAGE_META(mp)  ((BfKVMeta*)((u8*)(mp) + sizeof(BfMiniPage)))

/* Compute pointer to data area end (at page end) */
#define BF_MINI_PAGE_DATA_END(mp)  ((u8*)(mp) + (mp)->nodeSize)

/*
** Mapping table entry.
** Maps page number to its location (disk, mini-page, or full page).
*/
struct BfMapEntry {
  u8 locType;               /* BfPageLocation enum */
  u8 reserved[3];           /* Alignment */
  void *pPage;              /* Pointer if BF_LOC_MINI or BF_LOC_FULL */
  i64 diskOffset;           /* Base page disk offset */
};

/*
** Mapping table batch size.
** Entries are allocated in batches for efficiency.
*/
/* A batch is allocated whole the first time ANY page in its range is cached, so
** the batch size is a bet on locality.  Measured on a 6.8 GB database with a
** zipf read workload, the bet loses badly: 4,647 batches of 256 entries held
** 8,048 entries -- 0.7% occupancy, 27 MiB of mapping table for 2 MiB of live
** mini-pages.  Cached leaves scatter over the whole file, so a big batch mostly
** buys zeroes.  16 keeps the same structure and the same O(1) lookup while
** costing 384 bytes per touched range instead of 6144.
**
** MAX_BATCHES has to grow with the same factor to keep the addressable file
** size: 16 * 2^21 = 33,554,432 pages (128 GB @ 4 KB).  apMap is realloc'd to
** just past the highest batch touched, so this is a ceiling, not a commitment
** (1.67M-page file => ~105k pointers => 835 KiB). */
#define BF_MAP_BATCH_SIZE     16
#define BF_MAP_MAX_BATCHES    (1<<21)

/*
** Main Bf-Tree cache structure.
** Implements sqlite3_pcache interface for SQLite integration.
*/
struct BfCache {
  /* SQLite pcache interface compatibility */
  int szPage;               /* Full page size (typically 4096) */
  int szExtra;              /* Extra space per page for MemPage, PgHdr */
  int bPurgeable;           /* True if pages are on backing store */

  /* Core components */
  BfCircularBuffer cb;      /* Circular buffer for mini-pages */
  BfMapEntry **apMap;       /* Mapping table (array of batch pointers) */
  int nMapBatch;            /* Number of allocated batches */
  u32 nMaxPage;             /* Maximum cached pages (configured) */

  /* Size class configuration */
  u32 aSizeClass[BF_SIZE_CLASS_COUNT];

  /* Dirty page tracking */
  u32 nDirty;               /* Count of dirty mini-pages */

  /* Conservative "has buffered inserts" flag (SQLITE_BF_INSERT_BUFFERING).
  ** Set whenever a BFOP_INSERT is buffered; cleared only by the full
  ** commit-time flush (sqlite3BfBtreeFlushAllDirty) and by rollback
  ** (sqlite3BfBtreeClearCache).  Lets the pre-mutation / pre-scan flush
  ** (bfFlushTableDirty) early-out without an O(map) walk on read-only
  ** workloads after a commit — never reports clean while anything is dirty,
  ** so it can only skip a provably-empty flush.  (nDirty is unusable for this:
  ** it counts every write incl. clean and is never decremented.) */
  int bDirtyInserts;

  /* Eviction callback */
  int (*xStress)(void*, void*);  /* Callback to merge/write mini-pages */
  void *pStress;                  /* Argument to xStress */

  /* Configuration */
  /* NOTE: the read-promotion percentage is NOT stored per cache.  A field here
  ** was never assigned and silently disabled promotion everywhere; the single
  ** source of truth is bfConfig.nPromotionRate, reached through
  ** sqlite3BfCachePromotionRate().  Do not reintroduce a per-cache copy
  ** without also wiring PRAGMA bf_promotion_rate to update it. */

  /* Bypass flag: when non-zero, all BF hooks skip immediately.
  ** Used during merge flush to prevent re-entrant BF writes. */
  int bBypassActive;

  /* Dirty-list (perf): pgnos of mini-pages that MAY hold dirty records, so the
  ** flush/log paths iterate only those instead of walking the whole mapping
  ** table on every mutation and every commit.  Conservative in one direction
  ** only: a pgno may be stale (already clean -- dropped lazily during
  ** iteration), but a dirty mini-page is ALWAYS listed, because the only
  ** clean->dirty transition runs through sqlite3BfRecordWrite.  If the array
  ** cannot grow, bDirtyListOverflow forces the old full-map walk so a dirty
  ** mini-page can never be missed. */
  /* Max buffered rowid per table root, so OP_NewRowid can learn the true
  ** maximum key without materialising the table.  Monotonic and never cleared:
  ** after a flush the base max is >= this, and we always take the MAX of the
  ** two, so a stale entry can only ever skip a few rowids (allowed) -- it can
  ** never hand out a duplicate.  Overflowing the slots sets bMaxRowidUnknown,
  ** which puts every table back on the old flush-then-look path. */
  u32 aMaxRoot[BF_MAXROWID_SLOTS];
  i64 aMaxRowid[BF_MAXROWID_SLOTS];
  int nMaxRowid;
  int bMaxRowidUnknown;

  u32 *aDirtyPg;            /* pgnos that may have dirty records */
  int nDirtyPg;             /* entries in use */
  int nDirtyPgAlloc;        /* allocated slots */
  int bDirtyListOverflow;   /* 1 => list unusable, fall back to full walk */

  /* A mini-page the caller is mid-way through reading, which the FIFO eviction
  ** sweep must not reclaim.  sqlite3BfRecordWrite's upgrade and compaction
  ** paths evict-and-retry when the ring is full, and unlike the create path
  ** (where nothing is linked yet) their pEntry still points AT the slab they
  ** are about to copy FROM -- a clean one, which evictCallback would happily
  ** unlink and hand back to the ring, leaving the copy reading freed memory. */
  void *pEvictProtect;

  /* The record-cache lock.  Currently UNCONTENDED, and deliberately so: it is
  ** the prerequisite for sharing one cache across connections, not a fix for
  ** a race that exists today.
  **
  ** Today there is no race, because there is no sharing.  A BfCache is created
  ** per pager -- see sqlite3PagerOpenBfCache, "one BfCache per pager is the
  ** correct architecture" -- so each connection owns its own mapping table,
  ** mini-pages and ring.  (A bfGlobalCache singleton and its accessor looked like
  ** a shared singleton but have zero call sites; they are dead.)
  **
  ** That per-pager design is what makes multi-threading the REFERENCE workload
  ** wrong rather than unsafe: the reference runs 30 threads against ONE buffer
  ** pool, while N connections here would allocate N independent rings, each of
  ** bf_cache_size.  The memory budget the whole harness is built on would be
  ** overspent N-fold, or each thread would get 1/N of a cache.  Neither is the
  ** thing the paper measures.
  **
  ** So the faithful change is to share ONE cache per database file across
  ** connections -- and that is what needs this lock, because the moment two
  ** connections touch one cache, ordinary reads collide: sqlite3BfMiniPageSearch
  ** sets the REF bit with a read-modify-write on valueLenAndRef, the same u16
  ** that carries the value length and the WAL-logged flag, and
  ** sqlite3BfMapLookup walks a batch directory that bfMapEnsureCapacity
  ** reallocs under it.
  **
  ** RECURSIVE, because the public entry points nest (sqlite3BfRecordRead calls
  ** sqlite3BfMapLookup).  No inversion to design around: bf_cache.c never calls
  ** into the btree -- flushes are driven the other way, from bfFlushOneMiniPage
  ** in bf_btree.c -- and evictCallback refuses dirty pages rather than flushing
  ** them, so the critical section holds no I/O and never re-enters SQLite.
  **
  ** NOT YET SUFFICIENT for that sharing.  The BF API hands out raw pointers
  ** into cache state (sqlite3BfMapLookup returns a BfMapEntry*), so the lock
  ** has to span the CALLER's use of them: the scope is the 28 sqlite3BfBtree*
  ** entry points in btree.c, not the leaf functions.  Two of those -- the
  ** forward and reverse merge scans -- retain mini-page pointers ACROSS btree
  ** steps and need a pinning design before any lock makes them safe. */
  sqlite3_mutex *mutex;

  /* Pages holding dirty-but-unlogged records, i.e. what THIS transaction has
  ** to log at commit.  Same conservative contract as aDirtyPg above: entries
  ** may be stale or duplicated (revisiting a page whose records are all logged
  ** is a no-op), and on allocation failure bUnlogOverflow falls the commit back
  ** to the dirty list, which in turn falls back to the full map walk. */
  u32 *aUnlogPg;            /* pgnos that may have unlogged dirty records */
  int nUnlogPg;             /* entries in use */
  int nUnlogPgAlloc;        /* allocated slots */
  int bUnlogOverflow;       /* 1 => list unusable, fall back to aDirtyPg */

  /* Group commit (Phase 2): the open cross-transaction record batch, or NULL.
  ** Opaque here (BfGroup lives in bf_btree.c); freed by
  ** sqlite3BfBtreeGroupFree at cache teardown. */
  void *pGroupCommit;

  /* Back-pointer to the owning BtShared (set lazily on first btree access).
  ** Typed as void* to avoid exposing btreeInt.h here. */
  void *pBtShared;

  /* Root page number of the table currently being flushed.  Set by
  ** sqlite3BfBtreePrepareForScan before calling sqlite3BfCacheMergeWithPager
  ** and cleared afterwards.  Used by the apply callbacks to open the
  ** correct temporary write cursor for rowid tables. */
  u32 pgnoRootForFlush;

  /* Re-entrancy guard for the merge/flush path.  Set to 1 when a merge
  ** flush is in progress; prevents nested flushes from trying to open
  ** another cursor on the same B-tree. */
  int bMergingActive;

  /* Descent-shortcut suppression counter (Stage 1.6).  When non-zero the
  ** descent probe refuses to serve from the BF cache, forcing a real leaf
  ** read.  Used for range-seek descents and when materialising a BF-served
  ** cursor back onto its physical leaf. */
  int bShortcutSuppressed;

  /* Statistics */
  u64 nMiniPageHit;         /* Record lookups served from a mini-page */
  u64 nMiniPageMiss;        /* Record lookups that fell through to the page */
  u64 nPageFetchHit;        /* pcache2 xFetch served from the page hash */
  u64 nPageFetchMiss;       /* pcache2 xFetch that allocated/recycled a page */
  u64 nFullPageHit;         /* Lookups found in full page cache */
  u64 nMergeToBase;         /* Mini-pages merged to base page */
  u64 nEvictions;           /* Total evictions */
  u64 nUpgrades;            /* Mini-page size upgrades */
  u64 nMergeScans;          /* Forward scans that armed merge-iteration (2.2) */
  u64 nMergeInserts;        /* Buffered inserts emitted by merge-iteration */
  u64 nMergeBail;           /* Merge scans that fell back to flush + plain walk */
  u64 nWriteBackDeletes;    /* Deletes buffered as BFOP_DELETE tombstones (2.3) */
  u64 nMergeTombstones;     /* Base cells suppressed by a tombstone during merge */
  u64 nBufferedInserts;     /* Inserts absorbed by a mini-page (no base write) */
  u64 nInsertFallback;      /* Inserts that took the base-page write path anyway */
  u64 nCompactions;         /* Mini-pages compacted (clean records dropped to
                            ** make room for a dirty record) */
  u64 nInsertRefused;       /* Subset of the above: the mini-page REFUSED the
                            ** record (full at max size class / no usable leaf);
                            ** the rest were excluded by the buffering gate in
                            ** sqlite3BtreeInsert before BF was even asked */
  u64 nUpgradeShed;         /* Cold cache records dropped by size upgrades */
  /* Per-seek read outcome (H1b, 2026-09-24): the only honest record hit rate.
  ** nMiniPageHit/Miss count LOOKUPS, and one point read makes several (descent
  ** shortcut, the RecordExists at the leaf, fetchPayload's RecordExists, each
  ** counted again by its wrapper) -- a miss ~1.8 times, a hit about once -- so
  ** hits/(hits+misses) understated every hit rate this project has quoted.
  ** These count once per read-cursor rowid seek that descended:
  **   nSeekServed  the descent shortcut served the row; the leaf was NOT read
  **   nSeekLeaf    the seek reached the leaf page (whatever BF then found) */
  u64 nSeekServed;
  u64 nSeekLeaf;
  /* D3 (2026-09-29): existing-row UPDATE buffering and the shadow it creates.
  **   nBufferedUpdates  loc==0 rowid writes absorbed as a BFOP_INSERT upsert
  **   nUpdateFallbacks  eligible updates the mini-page refused (base write)
  **   nShadowServes     exact-match seeks that found a base cell AND a newer
  **                     dirty BFOP_INSERT for it, and parked on the latter */
  u64 nBufferedUpdates;
  u64 nUpdateFallbacks;
  u64 nShadowServes;

  /* Record-op replay ordering (D1).  Replay rebuilds the cache from the WAL's
  ** record ops at recovery and after a rollback.  An op is skipped when a later
  ** CLEAR of its leaf says a flush already put it in base pages (superseded),
  ** or when its frame is past the committed end of the log (torn tail); an op
  ** the cache refuses is counted as dropped, which must stay 0.
  ** nRollbackRehydrate counts rollbacks that rebuilt the cache that way
  ** instead of just emptying it; nClearLogged the CLEAR ops written. */
  /* Read-promotion coin (xorshift32).  Deterministic on purpose: it used to
  ** be sqlite3_randomness, seeded from the OS, so the SAME script promoted
  ** different rows on every run -- a cache-state-dependent bug then showed up
  ** in 9 of 40 runs, could not be minimised, and a green differential run
  ** proved less than it seemed.  0 means "not seeded yet". */
  u32 promoteRng;
  u64 nStaleCacheDrop;      /* write-through could not cache the new value,
                            ** so the leaf's clean records were dropped */

  u64 nReplayApplied;
  u64 nReplaySuperseded;
  u64 nReplayTorn;
  u64 nReplayDropped;       /* replayed ops the cache REFUSED (full mini-page):
                            ** nonzero means a committed record was lost */
  u64 nRollbackRehydrate;
  u64 nClearLogged;

  /* Leaves whose buffered records a flush applied to base pages in the
  ** CURRENT write transaction, as (leaf, root) pairs; the commit logs a
  ** BFWAL_OP_CLEAR for each (sqlite3BfBtreeLogAllDirty) and empties the list,
  ** a rollback just empties it.  WAL mode only.  bFlushedOom: the list could
  ** not grow, so the commit must fail rather than log an incomplete set. */
  u32 *aFlushed;
  int nFlushed;
  int nFlushedAlloc;
  u8 bFlushedOom;

  /* D3b blind insert (2026-09-29): a bitmap over pgno of pages KNOWN to be
  ** non-root table leaves, so an upsert can buffer against a child pgno
  ** without reading the child.  SQLite interior pages do not record their
  ** height, so this is how the descent tells "the next page is a leaf" from
  ** the parent alone.  Set when a descent enters a table leaf (moveToChild);
  ** a non-root page keeps its leaf-ness until it is freed, so the bit is
  ** cleared when the page is freed (ForgetPage) or allocated, for every page
  ** at or above an xTruncate limit (file shrink, pager reset after another
  ** connection's write), and wholesale by every rollback (ClearCache).  A bit
  ** is only ever a claim about the CURRENT snapshot. */
  u8 *aLeafBit;
  u32 nLeafBit;             /* bytes in aLeafBit */
  char *pSpareScratch;      /* one BF_MAX_MINI_PAGE cursor scratch kept across
                            ** cursor close/open (sqlite3BfBtreeScratchGet) */
  u64 nBlindInserts;        /* upserts buffered without reading the leaf */
  u64 nBlindNoLeaf;         /* armed, but the child was not a known leaf */
  u64 nBlindRefused;        /* reached a known leaf, but the mini-page refused */
  u64 nCopyOnAccess;        /* Mini-pages relocated to the tail on a read hit
                            ** (the second chance; PRAGMA bf_copy_on_access) */
  u64 nCopyOnAccessShed;    /* Cold cache records dropped by those relocations */

  /* Flush-capable dirty eviction (M1).  evictCallback cannot flush a dirty
  ** mini-page -- it has no Btree, no write transaction, and it runs inside the
  ** allocator -- so instead it RECORDS the leaf that aborted the sweep here and
  ** the btree layer drains it from a call point that has all three.  See
  ** sqlite3BfBtreeRelieveEvictStall(). */
  u32 pgnoEvictStall;       /* Leaf whose dirty mini-page aborted the last sweep */
  u64 nEvictStallSeen;      /* Times the sweep hit a mapped DIRTY slab and gave up.
                            ** This is the one that says whether M1 has a target
                            ** at all: if it is 0, no flush was ever possible and
                            ** the other three counters are 0 for that reason. */
  u64 nDirtyEvictFlush;     /* Stalls relieved by flushing that leaf to base */
  u64 nDirtyEvictRetryOk;   /* Buffered inserts rescued by such a flush */
  u64 nDirtyEvictRefused;   /* Stalls seen but unflushable (no root, no write txn) */
};

/*
** Return codes for Bf-Tree operations.
*/
#define BF_OK               0
#define BF_ERROR            1
#define BF_FULL             2    /* Circular buffer full */
#define BF_NOT_FOUND        3    /* Record not found in mini-page */
#define BF_DELETED          5    /* Record was deleted or is a phantom */
#define BF_MINI_PAGE_FULL   4    /* Mini-page full, needs upgrade or merge */

/*
** Initialize the Bf-Tree cache subsystem.
** Call once at SQLite initialization.
*/

/*
** Create and destroy Bf-Tree cache instances.
*/
SQLITE_PRIVATE BfCache *sqlite3BfCacheCreate(int szPage, int szExtra, int bPurgeable);
SQLITE_PRIVATE void sqlite3BfCacheDestroy(BfCache *pCache);

/*
** Configure cache size.
*/

/*
** Circular buffer operations.
*/
SQLITE_PRIVATE int sqlite3BfCircularBufferInit(BfCircularBuffer *pCb, u64 capacity);
SQLITE_PRIVATE void sqlite3BfCircularBufferDestroy(BfCircularBuffer *pCb);
SQLITE_PRIVATE void *sqlite3BfCircularBufferAlloc(BfCircularBuffer *pCb, u32 size);
SQLITE_PRIVATE int sqlite3BfCircularBufferDealloc(BfCircularBuffer *pCb, void *ptr);
SQLITE_PRIVATE int sqlite3BfCircularBufferEvictOne(BfCircularBuffer *pCb,
    int (*xEvict)(void*, void*), void *pCtx);
SQLITE_PRIVATE int sqlite3BfCircularBufferEvictN(BfCircularBuffer *pCb, int nTarget,
    int (*xEvict)(void*, void*), void *pCtx);
SQLITE_PRIVATE int sqlite3BfCircularBufferIsCopyOnAccess(BfCircularBuffer *pCb, void *ptr);
SQLITE_PRIVATE void sqlite3BfCircularBufferSetCopyOnAccess(BfCircularBuffer *pCb, double r);

/*
** Mini-page operations.
*/
SQLITE_PRIVATE void sqlite3BfMiniPageInit(BfMiniPage *pMini, u16 size, i64 baseDiskOffset);
SQLITE_PRIVATE int sqlite3BfMiniPageInsert(BfMiniPage *pMini,
    const void *pKey, int nKey, const void *pVal, int nVal, u8 opType);
/* Look up an exact key and report its raw BFOP_* op type (does NOT collapse
** DELETE/PHANTOM into BF_DELETED).  Returns 1 + sets *pOp if present, else 0. */
SQLITE_PRIVATE int sqlite3BfMiniPageLookupOp(BfMiniPage *pMini,
    const void *pKey, int nKey, u8 *pOp);
SQLITE_PRIVATE int sqlite3BfMiniPageSearch(BfMiniPage *pMini,
    const void *pKey, int nKey, void *pBuf, int *pnBuf);
/* Fill aSizeClass[BF_SIZE_CLASS_COUNT] with the derived ladder, ascending.
** ONE definition: BfFreeList and BfCache each keep a copy, and if the two ever
** disagreed a block allocated from one class would be freed into another. */
SQLITE_PRIVATE void sqlite3BfInitSizeClasses(u32 *aSizeClass, u32 nMinRecord);
SQLITE_PRIVATE int sqlite3BfCacheMinRecord(void);
SQLITE_PRIVATE double sqlite3BfCacheCopyOnAccessRatio(void);
SQLITE_PRIVATE u32 sqlite3BfMiniPageSizeClassFor(BfMiniPage *pMini, int nKey,
                                                 int nVal, u32 *aSizeClass);

/*
** Mapping table operations.
*/
SQLITE_PRIVATE BfMapEntry *sqlite3BfMapLookup(BfCache *pCache, u32 pgno);
SQLITE_PRIVATE int sqlite3BfMapInsert(BfCache *pCache, u32 pgno, u8 locType, void *pPage, i64 diskOffset);

/*
** Cache operations (high-level interface).
*/
SQLITE_PRIVATE int sqlite3BfCacheEvict(BfCache *pCache, int nTarget);

/*
** Record-level operations (bypass full page for point queries).
*/
SQLITE_PRIVATE int sqlite3BfRecordRead(BfCache *pCache, u32 pgno,
    const void *pKey, int nKey, void *pBuf, int *pnBuf);
SQLITE_PRIVATE int sqlite3BfRecordWrite(BfCache *pCache, u32 pgno,
    const void *pKey, int nKey, const void *pVal, int nVal, u8 opType);
/* Phase 2 (WAL) recovery / rollback: replay a pgno->ops index back into the
** mini-page cache (records written back dirty, unlogged).  Each leaf starts
** after its last CLEAR; ops past mxFrame are skipped unless bKeepBeyond.  See
** bf_cache.c. */
typedef struct BfWalIndex BfWalIndex;
SQLITE_PRIVATE int sqlite3BfCacheReplayWal(BfCache *pCache, BfWalIndex *pWalIdx,
    u32 mxFrame, int bKeepBeyond);

/*
** Pluggable cache interface for SQLite integration.
** Implements sqlite3_pcache_methods2.
*/
SQLITE_PRIVATE void sqlite3BfCacheSetMethods(void);

/*
** Additional circular buffer operations.
*/
SQLITE_PRIVATE void sqlite3BfCircularBufferMarkReady(void *ptr);
SQLITE_PRIVATE void sqlite3BfCircularBufferStats(BfCircularBuffer *pCb,
    u64 *pUsed, u64 *pCapacity, u64 *pAllocs, u64 *pEvictions);

/*
** Additional mini-page operations.
*/
SQLITE_PRIVATE int sqlite3BfMiniPageDirtyCount(BfMiniPage *pMini);
SQLITE_PRIVATE int sqlite3BfMiniPageIsDirty(BfMiniPage *pMini);
SQLITE_PRIVATE void sqlite3BfMiniPageMarkClean(BfMiniPage *pMini);
SQLITE_PRIVATE int sqlite3BfKvIsColdCache(const BfKVMeta *pMeta);
SQLITE_PRIVATE void sqlite3BfCacheCopyOnAccess(BfCache *pCache, BfMapEntry *pEntry);
/* A3a: take/release the record-cache lock.  No-ops in a single-threaded build,
** and tolerant of a null cache so call sites need no extra guard. */
#if SQLITE_THREADSAFE
# define bfCacheEnter(p)  do{ if( (p) && (p)->mutex ) sqlite3_mutex_enter((p)->mutex); }while(0)
# define bfCacheLeave(p)  do{ if( (p) && (p)->mutex ) sqlite3_mutex_leave((p)->mutex); }while(0)
#else
# define bfCacheEnter(p)  do{ }while(0)
# define bfCacheLeave(p)  do{ }while(0)
#endif

/* copyMode values for sqlite3BfMiniPageCopy. */
#define BF_COPY_ALL        0   /* every record (size-class upgrade) */
#define BF_COPY_REFERENCED 1   /* drop cold cache records (copy-on-access) */
#define BF_COPY_DIRTY      2   /* only BFOP_INSERT/BFOP_DELETE (compaction) */
SQLITE_PRIVATE int sqlite3BfMiniPageCopy(BfMiniPage *pDst, u16 dstSize,
    BfMiniPage *pSrc, int copyMode);
/* Drop clean (cache/phantom) records, keep dirty ones — see the definition. */
SQLITE_PRIVATE int sqlite3BfMiniPageDropClean(BfMiniPage *pMini);
SQLITE_PRIVATE int sqlite3BfMiniPageIterate(BfMiniPage *pMini,
    int (*xCallback)(void*, const u8*, int, const u8*, int, u8), void *pCtx);
/* Number of records (any op type) in the sorted meta array. */
SQLITE_PRIVATE int sqlite3BfMiniPageCount(BfMiniPage *pMini);
/* Random access to the record at sorted index ix (0-based).  Returns 1 and
** fills the requested out-params, or 0 if ix is out of range.  The returned
** key/value pointers reference the mini-page's own storage and are valid only
** until the next mutation of pMini (merge-scan reads them between steps). */
SQLITE_PRIVATE int sqlite3BfMiniPageAt(BfMiniPage *pMini, int ix,
    const u8 **ppKey, int *pnKey, const u8 **ppVal, int *pnVal, u8 *pOp);
/* Phase 2 (WAL) commit-gather: report record ix only if it is dirty AND not yet
** WAL-logged this generation (fills out-params, returns 1); mark record ix as
** logged once its op has been staged into a record-batch.  See bf_mini_page.c. */
SQLITE_PRIVATE int sqlite3BfMiniPageDirtyUnloggedAt(BfMiniPage *pMini, int ix,
    const u8 **ppKey, int *pnKey, const u8 **ppVal, int *pnVal, u8 *pOp);
SQLITE_PRIVATE void sqlite3BfMiniPageMarkLoggedAt(BfMiniPage *pMini, int ix);

/*
** Additional mapping operations.
*/
SQLITE_PRIVATE int sqlite3BfMapInit(BfCache *pCache);
SQLITE_PRIVATE void sqlite3BfMapDestroy(BfCache *pCache);
SQLITE_PRIVATE BfMapEntry *sqlite3BfMapGetOrCreate(BfCache *pCache, u32 pgno);
SQLITE_PRIVATE void sqlite3BfMapSpaceStats(BfCache *pCache, u64 *pnBatches,
                                           u64 *pnEntries, u64 *pnMiniPages,
                                           u64 *pnRecords, u64 *pnMiniBytes,
                                           u64 *pnCapacity);
SQLITE_PRIVATE int sqlite3BfMapIterate(BfCache *pCache,
    int (*xCallback)(void*, u32, BfMapEntry*), void *pCtx);

/*
** Pager integration functions.
*/
SQLITE_PRIVATE BfCache *sqlite3PagerGetBfCache(Pager *pPager);
SQLITE_PRIVATE int sqlite3BfPagerRecordRead(Pager *pPager, Pgno pgno,
    const void *pKey, int nKey, void *pBuf, int *pnBuf);
SQLITE_PRIVATE int sqlite3BfPagerRecordWrite(Pager *pPager, Pgno pgno,
    const void *pKey, int nKey, const void *pVal, int nVal, int isDelete);

/*
** Btree integration functions.
*/
SQLITE_PRIVATE int sqlite3BfBtreeFetchPayload(BtCursor *pCur, u32 offset, u32 amt, void *pBuf);
SQLITE_PRIVATE int sqlite3BfBtreeInsertCell(BtCursor *pCur, const void *pKey, int nKey,
    const void *pData, int nData);
SQLITE_PRIVATE int sqlite3BfBtreeDeleteCell(BtCursor *pCur, const void *pKey, int nKey);
SQLITE_PRIVATE int sqlite3BfBtreePrepareForScan(BtCursor *pCur);
SQLITE_PRIVATE int sqlite3BfBtreeRecordExists(BtCursor *pCur, const void *pKey, int nKey);
SQLITE_PRIVATE int sqlite3BfBtreePromoteRecord(BtCursor *pCur, const void *pKey, int nKey,
    const void *pData, int nData);
SQLITE_PRIVATE int sqlite3BfBtreeCachePhantom(BtCursor *pCur, const void *pKey, int nKey);
/* Write-through: mirror a rowid record into the leaf's read cache, called by
** the insert path BEFORE the base write while the cursor is on the target
** leaf (overwrites any stale CACHE/PHANTOM for the key; split-safe). */
SQLITE_PRIVATE int sqlite3BfBtreeCacheRecord(BtCursor *pCur, i64 rowid,
    const void *pData, int nData);
/* Drop any BF mini-page keyed by pgno (called when a page is freed/recycled). */
SQLITE_PRIVATE void sqlite3BfBtreeForgetPage(BtShared *pBt, Pgno pgno);
/* Descent shortcut (Stage 1.6): true iff leaf pgno chldPg has a CLEAN cached
** record for intKey, so the descent can skip reading the leaf page. */
SQLITE_PRIVATE int sqlite3BfBtreeDescentServe(BtCursor *pCur, Pgno chldPg,
    i64 intKey, void *pBuf, int *pnBuf);
/* Length of the clean cached record for the BF-served cursor's leaf key, or -1. */
/* Copy the clean cached record into pBuf (cap nCap) in one read; len or -1. */
SQLITE_PRIVATE int sqlite3BfBtreeReadCachedRecord(BtCursor *pCur, void *pBuf, int nCap);
SQLITE_PRIVATE void sqlite3BfEncodeRowid(i64 rowid, u8 *pBuf);
SQLITE_PRIVATE int sqlite3BfBtreeScratchGet(BtCursor *pCur);
SQLITE_PRIVATE void sqlite3BfBtreeScratchPut(BtCursor *pCur);
/* Re-descend a BF-served (BTCF_BfLeaf) cursor onto its real leaf page. */
SQLITE_PRIVATE int sqlite3BfBtreeMaterializeLeaf(BtCursor *pCur);
/* Adjust the descent-shortcut suppression counter (+1 suppress, -1 release). */
SQLITE_PRIVATE void sqlite3BfBtreeSuppressShortcut(BtCursor *pCur, int delta);
/* Arm forward merge-iteration for a full scan (Stage 2.2); 1 if armed. */
SQLITE_PRIVATE int sqlite3BfBtreeBeginMergeScan(BtCursor *pCur);
/* Arm reverse merge-iteration for a backward scan (Stage 2.4); 1 if armed. */
SQLITE_PRIVATE int sqlite3BfBtreeBeginMergeScanRev(BtCursor *pCur);
/* 1 iff the cursor is eligible for merge-iteration (no state change). */
SQLITE_PRIVATE int sqlite3BfBtreeCanMergeScan(BtCursor *pCur);
/* Dirty-list maintenance (perf): note that `pgno` may now hold dirty records. */
SQLITE_PRIVATE void sqlite3BfDirtyListAdd(BfCache *pCache, u32 pgno);
/* Iterate only the mini-pages that may be dirty, dropping stale entries.
** Returns 1 if the caller must fall back to sqlite3BfMapIterate (overflow). */
/* The unlogged-page list: add a pgno, walk it (returns non-zero if the caller
** must fall back to the dirty list), and drop every entry.  See BfCache. */
SQLITE_PRIVATE void sqlite3BfUnlogListAdd(BfCache *pCache, u32 pgno);
SQLITE_PRIVATE int sqlite3BfUnlogListIterate(BfCache *pCache,
    int (*xCallback)(void*, u32, BfMapEntry*), void *pCtx);
SQLITE_PRIVATE void sqlite3BfUnlogListReset(BfCache *pCache);

SQLITE_PRIVATE int sqlite3BfDirtyListIterate(BfCache *pCache,
    int (*xCallback)(void*, u32, BfMapEntry*), void *pCtx);
#if defined(SQLITE_BF_INSERT_BUFFERING)
/* Merge-scan (Stage 2.2): next buffered insert on `leaf` at index >= *pIx. */
SQLITE_PRIVATE int sqlite3BfBtreeMergeNextInsert(BtCursor *pCur, Pgno leaf,
    int *pIx, i64 *pRowid, void *pBuf, int nCap, int *pnVal);
/* Merge-scan (Stage 2.4): previous buffered insert on `leaf` at index <= *pIx. */
SQLITE_PRIVATE int sqlite3BfBtreeMergePrevInsert(BtCursor *pCur, Pgno leaf,
    int *pIx, i64 *pRowid, void *pBuf, int nCap, int *pnVal);
/* Write-amp accounting: an insert took the base-page path (not buffered). */
SQLITE_PRIVATE void sqlite3BfBtreeNoteInsertFallback(BtCursor *pCur);
/* Largest rowid buffered for this cursor's table; 1 if known, 0 if not. */
SQLITE_PRIVATE int sqlite3BfBtreeMaxBufferedRowid(BtCursor *pCur, i64 *pMax);
/* Group commit (Phase 2): force the open cross-transaction record batch into
** the current transaction's WAL frame stream; free it at cache teardown. */
SQLITE_PRIVATE void sqlite3BfBtreeGroupStagePending(Btree *p);
SQLITE_PRIVATE void sqlite3BfBtreeGroupFree(BfCache *pBf);
/* Transactions per record-frame group (PRAGMA bf_group_commit; 0/1 = off). */
SQLITE_PRIVATE int sqlite3BfGroupCommitTxns(void);
/* Merge-scan (Stage 2.2): flush table + drop merge state, revert to plain. */
SQLITE_PRIVATE int sqlite3BfBtreeMergeBail(BtCursor *pCur);
#if !defined(SQLITE_BF_NO_WRITEBACK_DELETE)
/* Write-back delete (Stage 2.3): buffer a BFOP_DELETE tombstone for the cursor's
** current leaf+key; SQLITE_OK if buffered, SQLITE_FULL to fall through. */
SQLITE_PRIVATE int sqlite3BfBtreeBufferDelete(BtCursor *pCur,
    const void *pKey, int nKey);
/* Write-back delete (Stage 2.3): 1 iff rowid is tombstoned on leaf (DELETE,
** not a clean PHANTOM) — the merge scan suppresses that base cell. */
SQLITE_PRIVATE int sqlite3BfBtreeKeyTombstoned(BtCursor *pCur, Pgno leaf,
    i64 rowid);
#endif
#if defined(SQLITE_BF_INSERT_BUFFERING)
/* D3-core: the DIRTY op (BFOP_INSERT / BFOP_DELETE) buffered for rowid on leaf,
** or -1 when the leaf holds no dirty record for it. */
SQLITE_PRIVATE int sqlite3BfBtreeKeyDirtyOp(BtCursor *pCur, Pgno leaf, i64 rowid);
/* D3b: the known-leaf bitmap (BfCache.aLeafBit) and the blind upsert. */
SQLITE_PRIVATE void sqlite3BfLeafBitSet(BfCache *pBf, u32 pgno);
SQLITE_PRIVATE void sqlite3BfLeafBitClear(BfCache *pBf, u32 pgno);
SQLITE_PRIVATE void sqlite3BfLeafBitClearFrom(BfCache *pBf, u32 pgno);
SQLITE_PRIVATE int sqlite3BfLeafBitTest(BfCache *pBf, u32 pgno);
SQLITE_PRIVATE void sqlite3BfBtreeNoteLeaf(BtCursor *pCur);
SQLITE_PRIVATE void sqlite3BfBtreeNotePageAllocated(BtShared *pBt, Pgno pgno);
SQLITE_PRIVATE void sqlite3BfBtreeNoteBlindMiss(BtCursor *pCur);
SQLITE_PRIVATE int sqlite3BfBtreeBlindChild(BtCursor *pCur, Pgno chldPg);
SQLITE_PRIVATE int sqlite3BfBtreeBlindInsert(BtCursor *pCur, i64 rowid,
    const void *pData, int nData);
/* D3a: buffer an existing-row overwrite as a BFOP_INSERT upsert on the cursor's
** leaf; SQLITE_OK if buffered, SQLITE_FULL to fall through to the base write. */
SQLITE_PRIVATE int sqlite3BfBtreeUpdateCell(BtCursor *pCur, i64 rowid,
    const void *pData, int nData);
SQLITE_PRIVATE void sqlite3BfBtreeNoteShadowServe(BtCursor *pCur);
#endif
#endif
#ifdef SQLITE_DEBUG
/* Debug-only: 1 if a BF mini-page is mapped under pgno (relocatePage assert). */
SQLITE_PRIVATE int sqlite3BfBtreeDebugHasMiniPage(BtShared *pBt, Pgno pgno);
#endif
/* Flush all dirty mini-pages for a B-tree to base pages (called at commit). */
SQLITE_PRIVATE int sqlite3BfBtreeFlushAllDirty(Btree *p);
#if defined(SQLITE_BF_INSERT_BUFFERING)
/* Phase 2 (WAL): gather dirty-but-unlogged records into WAL record-batch frames
** and stage them for the pending commit, WITHOUT writing base pages (WAL mode
** only).  Records stay dirty in the cache, materialised to base at checkpoint. */
SQLITE_PRIVATE int sqlite3BfBtreeLogAllDirty(Btree *p);
#endif
/* Flush a table's dirty mini-pages before a base mutation that may rebalance
** or free its leaves (Phase 1 per-leaf survival).  Returns non-zero if work
** was done (caller should re-seek its cursor). */
SQLITE_PRIVATE int sqlite3BfBtreeFlushTableForMutation(BtCursor *pCur);
SQLITE_PRIVATE int sqlite3BfBtreeRelieveEvictStall(BtCursor *pCur);
SQLITE_PRIVATE void sqlite3BfBtreeNoteDirtyEvictRetry(BtCursor *pCur);
SQLITE_PRIVATE void sqlite3BfBtreeDirtyEvictStat(Btree*, u64*, u64*, u64*, u64*);
/* Discard all BF state for a B-tree (called at rollback). */
SQLITE_PRIVATE void sqlite3BfBtreeClearCache(Btree *p);
/* Full ROLLBACK in WAL mode: empty the cache, then rebuild its committed dirty
** state from the log (consumes apStage).  See bf_btree.c. */
SQLITE_PRIVATE int sqlite3BfBtreeRollbackRehydrate(Btree *p, u8 **apStage,
                                                   int nStage);
/* Checkpoint's forced-flush commit: log the CLEARs its flush produced. */
SQLITE_PRIVATE int sqlite3BfBtreeLogFlushMarks(Btree *p);

/*
** P1: Record application during merge (Core behavioral alignment).
** Apply cached mini-page records to btree base pages during merge.
*/
SQLITE_PRIVATE int sqlite3BfBtreeApplyInsert(Pager *pPager, Pgno pgno,
    const void *pKey, int nKey, const void *pVal, int nVal);
SQLITE_PRIVATE int sqlite3BfBtreeApplyDelete(Pager *pPager, Pgno pgno,
    const void *pKey, int nKey);

/*
** Configuration functions.
*/
SQLITE_PRIVATE int sqlite3BfCacheEnabled(void);
SQLITE_PRIVATE u64 sqlite3BfCacheBufferSize(void);
SQLITE_PRIVATE int sqlite3BfCachePromotionRate(void);

/*
** Per-pager cache management (P0.3).
*/
typedef struct Pager Pager;  /* Forward declaration */
SQLITE_PRIVATE BfCache *sqlite3BfGetPagerCache(Pager *pPager);
SQLITE_PRIVATE void sqlite3BfClosePagerCache(Pager *pPager);

/*
** Configuration option numbers (add to sqlite3.h SQLITE_CONFIG_* list).
*/
/* SQLITE_CONFIG_BFCACHE / _SIZE used to be defined here as 30 and 31.
** Removed 2026-09-21 with their (never-called) handlers.  They were a latent
** collision, not merely dead: 30 is SQLite's own SQLITE_CONFIG_ROWID_IN_VIEW,
** so wiring these up as written would have broken a real config option.  Every
** BF setting is reachable through its PRAGMA; if a sqlite3_config() entry point
** is ever wanted, allocate opcodes that upstream does not use. */

#endif /* _BF_CACHE_H_ */
