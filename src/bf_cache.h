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
#define BF_SIZE_CLASS_COUNT   7    /* 64, 128, 256, 512, 1024, 2048, 4096 */

/*
** Default configuration values.
**
** BF_DEFAULT_BUFFER_SIZE: the circular buffer backing the mini-page pool.
** Must be a power of 2 and >= BF_MAX_MINI_PAGE + sizeof(BfAllocMeta).
** 8 MB gives ~116k 64-byte mini-page slots, adequate for a benchmark
** database with thousands of hot records.
*/
#define BF_DEFAULT_BUFFER_SIZE    (8*1024*1024)   /* 8 MB circular buffer */
#define BF_DEFAULT_COPY_ON_ACCESS 0.1             /* 10% copy-on-access region */
#define BF_DEFAULT_PROMOTION_RATE 5               /* 5% read promotion rate */

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
struct BfMiniPage {
  u16 nodeSize;             /* Total size of this mini-page */
  u16 metaCount;            /* Number of records (including fence keys) */
  u16 freeSpace;            /* Free space between meta array and data */
  u16 prefixLen;            /* Common prefix length for keys (compression) */
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
#define BF_MAP_BATCH_SIZE     256            /* Small batch to keep allocations bounded */
#define BF_MAP_MAX_BATCHES    65536          /* 256 * 65536 = 16,777,216 pages (64 GB @ 4 KB) */

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
  int promotionRate;        /* Percentage (0-100) to cache read records */

  /* Bypass flag: when non-zero, all BF hooks skip immediately.
  ** Used during merge flush to prevent re-entrant BF writes. */
  int bBypassActive;

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
  u64 nMiniPageHit;         /* Lookups found in mini-page */
  u64 nMiniPageMiss;        /* Lookups not found, went to disk */
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
SQLITE_PRIVATE int sqlite3BfCacheInit(void);
SQLITE_PRIVATE void sqlite3BfCacheShutdown(void);

/*
** Create and destroy Bf-Tree cache instances.
*/
SQLITE_PRIVATE BfCache *sqlite3BfCacheCreate(int szPage, int szExtra, int bPurgeable);
SQLITE_PRIVATE void sqlite3BfCacheDestroy(BfCache *pCache);

/*
** Configure cache size.
*/
SQLITE_PRIVATE void sqlite3BfCacheSetCachesize(BfCache *pCache, int nMax);
SQLITE_PRIVATE int sqlite3BfCachePagecount(BfCache *pCache);

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
SQLITE_PRIVATE int sqlite3BfMiniPageDelete(BfMiniPage *pMini, const void *pKey, int nKey);
SQLITE_PRIVATE int sqlite3BfMiniPageNeedsMerge(BfMiniPage *pMini);
SQLITE_PRIVATE int sqlite3BfMiniPageSpaceRemaining(BfMiniPage *pMini);
SQLITE_PRIVATE u32 sqlite3BfMiniPageNextSizeClass(BfMiniPage *pMini, u32 *aSizeClass);

/*
** Mapping table operations.
*/
SQLITE_PRIVATE BfMapEntry *sqlite3BfMapLookup(BfCache *pCache, u32 pgno);
SQLITE_PRIVATE int sqlite3BfMapInsert(BfCache *pCache, u32 pgno, u8 locType, void *pPage, i64 diskOffset);
SQLITE_PRIVATE int sqlite3BfMapRemove(BfCache *pCache, u32 pgno);
SQLITE_PRIVATE int sqlite3BfMapUpdateLocation(BfCache *pCache, u32 pgno, u8 locType, void *pPage);

/*
** Cache operations (high-level interface).
*/
SQLITE_PRIVATE int sqlite3BfCacheFetch(BfCache *pCache, u32 pgno, int createFlag, void **ppPage);
SQLITE_PRIVATE int sqlite3BfCacheUnpin(BfCache *pCache, void *pPage, int reuseUnlikely);
SQLITE_PRIVATE int sqlite3BfCacheMakeDirty(BfCache *pCache, u32 pgno);
SQLITE_PRIVATE int sqlite3BfCacheMakeClean(BfCache *pCache, u32 pgno);
SQLITE_PRIVATE int sqlite3BfCacheMerge(BfCache *pCache, u32 pgno);
SQLITE_PRIVATE int sqlite3BfCacheEvict(BfCache *pCache, int nTarget);
SQLITE_PRIVATE int sqlite3BfCacheTruncate(BfCache *pCache, u32 iLimit);

/*
** Record-level operations (bypass full page for point queries).
*/
SQLITE_PRIVATE int sqlite3BfRecordRead(BfCache *pCache, u32 pgno,
    const void *pKey, int nKey, void *pBuf, int *pnBuf);
SQLITE_PRIVATE int sqlite3BfRecordWrite(BfCache *pCache, u32 pgno,
    const void *pKey, int nKey, const void *pVal, int nVal, u8 opType);
/* Phase 2 (WAL) recovery: replay the WAL's rebuilt pgno->ops index back into
** the mini-page cache (records written back dirty, unlogged).  See bf_cache.c. */
typedef struct BfWalIndex BfWalIndex;
SQLITE_PRIVATE int sqlite3BfCacheReplayWal(BfCache *pCache, BfWalIndex *pWalIdx);

/*
** Pluggable cache interface for SQLite integration.
** Implements sqlite3_pcache_methods2.
*/
SQLITE_PRIVATE void sqlite3BfCacheSetMethods(void);

/*
** Additional circular buffer operations.
*/
SQLITE_PRIVATE void sqlite3BfCircularBufferMarkReady(void *ptr);
SQLITE_PRIVATE u32 sqlite3BfCircularBufferGetSize(void *ptr);
SQLITE_PRIVATE void sqlite3BfCircularBufferStats(BfCircularBuffer *pCb,
    u64 *pUsed, u64 *pCapacity, u64 *pAllocs, u64 *pEvictions);

/*
** Additional mini-page operations.
*/
SQLITE_PRIVATE int sqlite3BfMiniPageDirtyCount(BfMiniPage *pMini);
SQLITE_PRIVATE int sqlite3BfMiniPageIsDirty(BfMiniPage *pMini);
SQLITE_PRIVATE void sqlite3BfMiniPageMarkClean(BfMiniPage *pMini);
SQLITE_PRIVATE void sqlite3BfMiniPageClearRefs(BfMiniPage *pMini);
SQLITE_PRIVATE int sqlite3BfMiniPageConsolidate(BfMiniPage *pMini);
SQLITE_PRIVATE int sqlite3BfMiniPageCopy(BfMiniPage *pDst, u16 dstSize,
    BfMiniPage *pSrc, int copyOnlyReferenced);
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
SQLITE_PRIVATE int sqlite3BfMapCount(BfCache *pCache);
SQLITE_PRIVATE int sqlite3BfMapMiniPageCount(BfCache *pCache);
SQLITE_PRIVATE int sqlite3BfMapFullPageCount(BfCache *pCache);
SQLITE_PRIVATE int sqlite3BfMapIterate(BfCache *pCache,
    int (*xCallback)(void*, u32, BfMapEntry*), void *pCtx);

/*
** Pager integration functions.
*/
SQLITE_PRIVATE int sqlite3PagerUsesBfCache(Pager *pPager);
SQLITE_PRIVATE BfCache *sqlite3PagerGetBfCache(Pager *pPager);
SQLITE_PRIVATE int sqlite3BfPagerRecordRead(Pager *pPager, Pgno pgno,
    const void *pKey, int nKey, void *pBuf, int *pnBuf);
SQLITE_PRIVATE int sqlite3BfPagerRecordWrite(Pager *pPager, Pgno pgno,
    const void *pKey, int nKey, const void *pVal, int nVal, int isDelete);
SQLITE_PRIVATE int sqlite3BfPagerMergeMiniPage(Pager *pPager, Pgno pgno);
SQLITE_PRIVATE int sqlite3BfPagerMergeAllMiniPages(Pager *pPager);
SQLITE_PRIVATE int sqlite3BfCacheMergeWithPager(BfCache *pCache, u32 pgno, Pager *pPager);

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
SQLITE_PRIVATE int sqlite3BfBtreeDescentProbe(BtCursor *pCur, Pgno chldPg, i64 intKey);
/* Length of the clean cached record for the BF-served cursor's leaf key, or -1. */
SQLITE_PRIVATE int sqlite3BfBtreeCachedPayloadSize(BtCursor *pCur);
/* Copy the clean cached record into pBuf (cap nCap) in one read; len or -1. */
SQLITE_PRIVATE int sqlite3BfBtreeReadCachedRecord(BtCursor *pCur, void *pBuf, int nCap);
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
#if defined(SQLITE_BF_INSERT_BUFFERING)
/* Merge-scan (Stage 2.2): next buffered insert on `leaf` at index >= *pIx. */
SQLITE_PRIVATE int sqlite3BfBtreeMergeNextInsert(BtCursor *pCur, Pgno leaf,
    int *pIx, i64 *pRowid, void *pBuf, int nCap, int *pnVal);
/* Merge-scan (Stage 2.4): previous buffered insert on `leaf` at index <= *pIx. */
SQLITE_PRIVATE int sqlite3BfBtreeMergePrevInsert(BtCursor *pCur, Pgno leaf,
    int *pIx, i64 *pRowid, void *pBuf, int nCap, int *pnVal);
/* Write-amp accounting: an insert took the base-page path (not buffered). */
SQLITE_PRIVATE void sqlite3BfBtreeNoteInsertFallback(BtCursor *pCur);
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
/* Discard all BF state for a B-tree (called at rollback). */
SQLITE_PRIVATE void sqlite3BfBtreeClearCache(Btree *p);

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
SQLITE_PRIVATE void sqlite3BfCacheSetPromotionRate(int rate);
SQLITE_PRIVATE BfCache *sqlite3BfGetGlobalCache(int szPage);
SQLITE_PRIVATE void sqlite3BfResetGlobalCache(void);

/*
** Per-pager cache management (P0.3).
*/
typedef struct Pager Pager;  /* Forward declaration */
SQLITE_PRIVATE BfCache *sqlite3BfGetPagerCache(Pager *pPager);
SQLITE_PRIVATE void sqlite3BfClosePagerCache(Pager *pPager);

/*
** Configuration option numbers (add to sqlite3.h SQLITE_CONFIG_* list).
*/
#define SQLITE_CONFIG_BFCACHE       30  /* int: enable bf-tree cache */
#define SQLITE_CONFIG_BFCACHE_SIZE  31  /* i64: circular buffer size */

#endif /* _BF_CACHE_H_ */
