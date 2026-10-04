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
** This file implements the main Bf-Tree cache module.
**
** The module has two distinct parts:
**
**   1. A sqlite3_pcache_methods2 implementation (bfCacheCreate/Fetch/Unpin/…)
**      that hands the pager FULL-size page buffers, i.e. a drop-in page cache.
**      This layer does NOT itself use mini-pages (bfCacheFetch leaves
**      pPage->pMiniPage == 0); it exists so btree.c/pager.c see the ordinary
**      fixed-page contract.
**
**   2. A record-level API (sqlite3BfRecordRead/Write, sqlite3BfCacheMerge*,
**      eviction) layered over the mapping table + the circular buffer of
**      variable-length mini-pages.  This is the actual Bf-Tree behaviour —
**      buffering writes and caching hot records at record granularity — and
**      it is driven from the btree.c hooks, not from the pcache path above.
**
** So mini-pages are an internal record-cache/write-buffer layer beside the
** fixed-page B-tree, not the buffers returned to the pager.  Keeping btree.c
** on fixed pages is a pragmatic adaptation (see BF_TREE_V2_PLAN.md "Future
** work — the faithful branch"), not a file-format compatibility requirement.
*/
#include "sqliteInt.h"
#include "bf_cache.h"
#include "bf_wal.h"    /* record-ops index replayed into the cache at recovery */

#ifndef SQLITE_OMIT_BF_CACHE

/* Context structure for the apply dirty record callback */
typedef struct BfApplyContext BfApplyContext;
struct BfApplyContext {
  Pager *pPager;     /* Pager instance */
  u32 pgno;          /* Page number */
};

/* Forward declaration for the callback function */
static int bfApplyDirtyRecordCallback(void *pCtx, const u8 *pKey, int nKey,
                                      const u8 *pVal, int nVal, u8 opType);

#ifdef SQLITE_BF_DEBUG
#include <fcntl.h>
#include <unistd.h>
static void bfAllocTrace(const char *zTag, const void *pPtr, int nSize){
  char zBuf[256];
  int n = snprintf(zBuf, sizeof(zBuf), "bfalloc: %s ptr=%p size=%d\n", zTag, pPtr, nSize);
  int fd = open("/home/yawd/Projects/tfg-stuff/sqlite/build/bf-allocs.log",
                O_WRONLY|O_CREAT|O_APPEND, 0644);
  if( fd>=0 ){ write(fd, zBuf, n); close(fd); }
}
#define BF_ALLOC_TRACE(t,p,n)  bfAllocTrace(t,p,n)
#else
#define BF_ALLOC_TRACE(t,p,n)  ((void)0)
#endif

/*
** Global state for the Bf-Tree cache module.
*/
static struct BfCacheGlobal {
  int isInit;                    /* True after initialization */
  sqlite3_mutex *mutex;          /* Global mutex */
  u64 nTotalAlloc;               /* Total bytes allocated */
  u64 nTotalPages;               /* Total pages in all caches */
} bfGlobal;

/*
** Page header structure for Bf-Tree pages.
** This embeds sqlite3_pcache_page as required by the interface.
*/
typedef struct BfPage BfPage;
struct BfPage {
  sqlite3_pcache_page page;      /* Base structure (pBuf, pExtra) */
  BfCache *pCache;               /* Owning cache */
  u32 pgno;                      /* Page number */
  u8 locType;                    /* Current location type */
  u8 flags;                      /* Page flags */
  u16 nRef;                      /* Reference count */
  BfMiniPage *pMiniPage;         /* Associated mini-page (if any) */
  BfPage *pNext;                 /* Hash chain */
  BfPage *pLruPrev, *pLruNext;   /* LRU list pointers */
};

#define BF_PAGE_DIRTY    0x01
#define BF_PAGE_PINNED   0x02

/*
** Extended BfCache structure with internal state.
*/
typedef struct BfCacheInt BfCacheInt;
struct BfCacheInt {
  BfCache base;                  /* Public structure */

  /* Hash table for page lookup */
  BfPage **apHash;               /* Hash table */
  int nHash;                     /* Hash table size */
  unsigned int iMaxKey;          /* Largest pgno currently in apHash */

  /* LRU list for unpinned pages */
  BfPage lru;                    /* LRU list anchor */
  int nRecyclable;               /* Pages on LRU list */

  /* Page pool */
  int nPage;                     /* Total pages in cache */
  int nPinned;                   /* Pinned pages */

  /* Configuration */
  int nMin;                      /* Minimum pages to keep */
  int nMax;                      /* Maximum pages */
};

/*
** Forward declarations.
*/
static void bfCacheRemoveFromHash(BfCacheInt *pCache, BfPage *pPage, int freeFlag);
static BfPage *bfCachePinPage(BfPage *pPage);

/*
** Initialize the global Bf-Tree cache module.
*/
static int bfCacheInit(void *pArg){
  UNUSED_PARAMETER(pArg);

  if( bfGlobal.isInit ) return SQLITE_OK;

  memset(&bfGlobal, 0, sizeof(bfGlobal));

#if SQLITE_THREADSAFE
  bfGlobal.mutex = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_PMEM);
#endif

  bfGlobal.isInit = 1;
  BF_ALLOC_TRACE("cache-init", 0, 0);
  return SQLITE_OK;
}

/*
** Shutdown the global Bf-Tree cache module.
*/
static void bfCacheShutdown(void *pArg){
  UNUSED_PARAMETER(pArg);
  BF_ALLOC_TRACE("cache-shutdown", 0, 0);
  memset(&bfGlobal, 0, sizeof(bfGlobal));
}

/*
** Grow the page hash table to nNew buckets (a power of two).
**
** The bucket count MUST track the page count.  It used to be fixed at 256 for
** the life of the cache, which turned every xFetch into a linear walk of
** nMax/256 pages: at a 256 MiB page cache (65536 pages) that is a 256-entry
** chain per lookup, and measures as a ~13x slowdown against pcache1 on a
** read-only workload.  pcache1 keeps roughly one bucket per page for the same
** reason; bfCacheFetch now calls this whenever nPage reaches nHash.
*/
#define BF_MAX_HASH (1<<22)          /* 4M buckets; far above any real nMax */

static int bfCacheResizeHash(BfCacheInt *pCache, int nNew){
  BfPage **apNew;
  int i;
  BfPage *pPage, *pNext;

  assert( nNew>0 && (nNew & (nNew-1))==0 );
  if( nNew<=pCache->nHash ) return SQLITE_OK;
  apNew = (BfPage**)sqlite3MallocZero(sizeof(BfPage*) * (i64)nNew);
  if( !apNew ) return SQLITE_NOMEM;   /* keep the old table; still correct */

  /* Rehash existing entries */
  for(i = 0; i < pCache->nHash; i++){
    for(pPage = pCache->apHash[i]; pPage; pPage = pNext){
      pNext = pPage->pNext;
      int h = pPage->pgno % nNew;
      pPage->pNext = apNew[h];
      apNew[h] = pPage;
    }
  }

  sqlite3_free(pCache->apHash);
  pCache->apHash = apNew;
  pCache->nHash = nNew;

  return SQLITE_OK;
}

/*
** Create a new Bf-Tree cache.
*/
static sqlite3_pcache *bfCacheCreate(int szPage, int szExtra, int bPurgeable){
  BfCacheInt *pCache;
  i64 sz;
  int rc;

  assert( (szPage & (szPage - 1)) == 0 && szPage >= 512 && szPage <= 65536 );
  assert( szExtra < 300 );

  sz = sizeof(BfCacheInt);
  pCache = (BfCacheInt*)sqlite3MallocZero(sz);
  if( !pCache ) return 0;
  BF_ALLOC_TRACE("cache-create", pCache, (int)sz);

  /* Initialize base structure */
  pCache->base.szPage = szPage;
  pCache->base.szExtra = szExtra;
  pCache->base.bPurgeable = bPurgeable;

#if SQLITE_THREADSAFE
  /* A3a: the record-cache lock.  Recursive because the public entry points
  ** nest (sqlite3BfRecordRead -> sqlite3BfMapLookup). */
  pCache->base.mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_RECURSIVE);
  if( pCache->base.mutex==0 ){
    BF_ALLOC_TRACE("cache-create-fail", pCache, (int)sz);
    sqlite3_free(pCache);
    return 0;
  }
#endif

  /* Initialize circular buffer */
  rc = sqlite3BfCircularBufferInit(&pCache->base.cb, sqlite3BfCacheBufferSize());
  if( rc != SQLITE_OK ){
    BF_ALLOC_TRACE("cache-create-fail", pCache, (int)sz);
    sqlite3_mutex_free(pCache->base.mutex);
    sqlite3_free(pCache);
    return 0;
  }

  /* Initialize mapping table */
  rc = sqlite3BfMapInit(&pCache->base);
  if( rc != SQLITE_OK ){
    BF_ALLOC_TRACE("cache-create-fail", pCache, (int)sz);
    sqlite3BfCircularBufferDestroy(&pCache->base.cb);
    sqlite3_mutex_free(pCache->base.mutex);
    sqlite3_free(pCache);
    return 0;
  }

  /* Size classes, ASCENDING.  The mini-page helpers scan this array forwards and
  ** take the first class that fits, so the order is not cosmetic: filled
  ** backwards, they returned 4096 every time.  Derived, and shared with the
  ** free list's copy -- if the two disagreed, a block allocated from one class
  ** would be freed into another. */
  sqlite3BfInitSizeClasses(pCache->base.aSizeClass,
                           (u32)sqlite3BfCacheMinRecord());

  /* Initialize hash table */
  rc = bfCacheResizeHash(pCache, 256);
  if( rc != SQLITE_OK ){
    BF_ALLOC_TRACE("cache-create-fail", pCache, (int)sz);
    sqlite3BfCircularBufferDestroy(&pCache->base.cb);
    sqlite3BfMapDestroy(&pCache->base);
    sqlite3_free(pCache);
    return 0;
  }

  /* Initialize LRU list anchor */
  pCache->lru.pLruPrev = &pCache->lru;
  pCache->lru.pLruNext = &pCache->lru;

  /* Set defaults */
  pCache->nMin = bPurgeable ? 10 : 0;
  pCache->nMax = 100;  /* Will be overridden by xCachesize */

  return (sqlite3_pcache*)pCache;
}

/*
** Set the cache size.
*/
static void bfCacheCachesize(sqlite3_pcache *p, int nMax){
  BfCacheInt *pCache = (BfCacheInt*)p;
  pCache->nMax = nMax;
  pCache->base.nMaxPage = nMax;
}

/*
** Return the number of pages in the cache.
*/
static int bfCachePagecount(sqlite3_pcache *p){
  BfCacheInt *pCache = (BfCacheInt*)p;
  return pCache->nPage;
}

/*
** Allocate a new page buffer.
*/
static BfPage *bfCacheAllocPage(BfCacheInt *pCache){
  BfPage *pPage;
  int szAlloc;

  szAlloc = sizeof(BfPage) + pCache->base.szPage + pCache->base.szExtra;
  pPage = (BfPage*)sqlite3Malloc(szAlloc);
  if( !pPage ){
    BF_ALLOC_TRACE("page-alloc-null", 0, (int)szAlloc);
    return 0;
  }

  /* Zero entire allocation so page buffer and extra space are initialized */
  memset(pPage, 0, (size_t)szAlloc);

  /* Set up page pointers */
  pPage->page.pBuf = (u8*)pPage + sizeof(BfPage);
  pPage->page.pExtra = (u8*)pPage->page.pBuf + pCache->base.szPage;
  pPage->pCache = &pCache->base;
  BF_ALLOC_TRACE("page-alloc", pPage, szAlloc);
  return pPage;
}

/*
** Free a page buffer.
*/
static void bfCacheFreePage(BfPage *pPage){
  BF_ALLOC_TRACE("page-free", pPage, pPage && pPage->pCache ? ((BfCache*)pPage->pCache)->szPage + ((BfCache*)pPage->pCache)->szExtra + (int)sizeof(BfPage) : 0);
  if( pPage->pMiniPage ){
    /* Deallocate mini-page from circular buffer */
    sqlite3BfCircularBufferDealloc(&pPage->pCache->cb, pPage->pMiniPage);
    pPage->pMiniPage = 0;
  }
  sqlite3_free(pPage);
}

/*
** Add a page to the hash table.
*/
static void bfCacheAddToHash(BfCacheInt *pCache, BfPage *pPage){
  int h = pPage->pgno % pCache->nHash;
  pPage->pNext = pCache->apHash[h];
  pCache->apHash[h] = pPage;
  if( pPage->pgno > pCache->iMaxKey ) pCache->iMaxKey = pPage->pgno;
}

/*
** Remove a page from the hash table.
*/
static void bfCacheRemoveFromHash(BfCacheInt *pCache, BfPage *pPage, int freeFlag){
  int h = pPage->pgno % pCache->nHash;
  BfPage **pp;

  for(pp = &pCache->apHash[h]; *pp; pp = &(*pp)->pNext){
    if( *pp == pPage ){
      *pp = pPage->pNext;
      break;
    }
  }

  /* Remove from LRU if present */
  if( pPage->pLruNext ){
    pPage->pLruPrev->pLruNext = pPage->pLruNext;
    pPage->pLruNext->pLruPrev = pPage->pLruPrev;
    pPage->pLruNext = 0;
    pPage->pLruPrev = 0;
    pCache->nRecyclable--;
  }

  pCache->nPage--;

  if( freeFlag ){
    bfCacheFreePage(pPage);
  }
}

/*
** Pin a page (remove from LRU list).
*/
static BfPage *bfCachePinPage(BfPage *pPage){
  if( pPage->pLruNext ){
    pPage->pLruPrev->pLruNext = pPage->pLruNext;
    pPage->pLruNext->pLruPrev = pPage->pLruPrev;
    pPage->pLruNext = 0;
    pPage->pLruPrev = 0;

    BfCacheInt *pCache = (BfCacheInt*)pPage->pCache;
    pCache->nRecyclable--;
    pCache->nPinned++;
  }
  pPage->flags |= BF_PAGE_PINNED;
  return pPage;
}

/*
** Fetch a page from the cache.
*/
static sqlite3_pcache_page *bfCacheFetch(
  sqlite3_pcache *p,
  unsigned int iKey,
  int createFlag
){
  BfCacheInt *pCache = (BfCacheInt*)p;
  BfPage *pPage;
  int h;

  /* Step 1: Look for existing page in hash table */
  h = iKey % pCache->nHash;
  for(pPage = pCache->apHash[h]; pPage; pPage = pPage->pNext){
    if( pPage->pgno == iKey ){
      /* Found - pin it and return */
      pPage->nRef++;
      /* A page-cache hit, NOT a mini-page hit.  These used to share
      ** nMiniPageHit, which made PRAGMA bf_cache_stats report the pcache hit
      ** rate instead of the record-cache hit rate — a number that stayed flat
      ** across an 8x sweep of bf_cache_size because it never depended on it. */
      pPage->pCache->nPageFetchHit++;
      return (sqlite3_pcache_page*)bfCachePinPage(pPage);
    }
  }

  /* Step 2: Not found */
  if( !createFlag ){
    return 0;
  }

  /* Step 3: Check if we can create a new page */
  if( createFlag == 1 ){
    /* Soft create - fail if at capacity */
    if( pCache->nPage >= pCache->nMax && pCache->nRecyclable == 0 ){
      return 0;
    }
  }

  /* Step 4: Try to recycle an unpinned page */
  if( pCache->nPage >= pCache->nMax ){
    BfPage *pRecycle = pCache->lru.pLruPrev;
    if( pRecycle != &pCache->lru ){
      /* Remove from LRU and hash */
      bfCacheRemoveFromHash(pCache, pRecycle, 0);
      /* Reuse the page structure.  The buffer is NOT zeroed: the pcache
      ** contract leaves a fetched page's content undefined (the pager reads
      ** it, or zeroes it itself past end-of-file), and pcache1 does not zero
      ** either.  This memset cost 4 KiB of stores per page-cache miss -- 7%
      ** of point-read cycles on this box's E-cores (2026-09-30 profile). */
      pPage = pRecycle;
    }else{
      pPage = 0;
    }
  }

  /* Step 5: Allocate new page if needed */
  if( !pPage ){
    pPage = bfCacheAllocPage(pCache);
    if( !pPage ) return 0;
  }

  /* Initialize the page */
  pPage->pgno = iKey;
  pPage->nRef = 1;
  pPage->flags = BF_PAGE_PINNED;
  pPage->pLruNext = 0;
  pPage->pLruPrev = 0;

  /* Zero the first word of pExtra.  pcache.c stores a PgHdr there and uses
  ** PgHdr.pPage==0 (the first field) to detect that the header must be
  ** re-initialized (see pcacheFetchFinish).  A recycled page still carries
  ** the previous page's PgHdr — old pgno, old PGHDR_* flags — and handing
  ** it out without this reset corrupts the database under cache spill.
  ** pcache1 does the same (pcache1FetchStage2). */
  *(void **)pPage->page.pExtra = 0;

  /* Record-level mini-pages are owned by the mapping table (see
  ** sqlite3BfRecordWrite).  Do NOT allocate one per pcache page here: the
  ** pointer is never read back, so it only floods the circular buffer with
  ** unreachable 64B slots and leaves a dangling reference once the FIFO
  ** eviction sweep reclaims them. */
  pPage->pMiniPage = 0;
  pPage->locType = BF_LOC_BASE;

  /* Add to hash table */
  bfCacheAddToHash(pCache, pPage);
  pCache->nPage++;
  pCache->nPinned++;

  /* Keep the chain length near 1.  Doubling is amortised O(1) and a failed
  ** allocation simply leaves the smaller table in place. */
  if( pCache->nPage >= pCache->nHash && pCache->nHash < BF_MAX_HASH ){
    (void)bfCacheResizeHash(pCache, pCache->nHash * 2);
  }

  pPage->pCache->nPageFetchMiss++;

  return (sqlite3_pcache_page*)pPage;
}

/*
** Unpin a page (make it eligible for recycling).
*/
static void bfCacheUnpin(
  sqlite3_pcache *p,
  sqlite3_pcache_page *pPg,
  int reuseUnlikely
){
  BfCacheInt *pCache = (BfCacheInt*)p;
  BfPage *pPage = (BfPage*)pPg;

  assert( pPage->pCache == &pCache->base );
  assert( pPage->flags & BF_PAGE_PINNED );

  pPage->nRef--;
  pPage->flags &= ~BF_PAGE_PINNED;
  pCache->nPinned--;

  if( reuseUnlikely || pCache->nPage > pCache->nMax ){
    /* Remove immediately */
    bfCacheRemoveFromHash(pCache, pPage, 1);
  }else{
    /* Add to LRU list (front = most recently used) */
    pPage->pLruNext = pCache->lru.pLruNext;
    pPage->pLruPrev = &pCache->lru;
    pCache->lru.pLruNext->pLruPrev = pPage;
    pCache->lru.pLruNext = pPage;
    pCache->nRecyclable++;
  }
}

/*
** Change the page number for a page.
*/
static void bfCacheRekey(
  sqlite3_pcache *p,
  sqlite3_pcache_page *pPg,
  unsigned int iOld,
  unsigned int iNew
){
  BfCacheInt *pCache = (BfCacheInt*)p;
  BfPage *pPage = (BfPage*)pPg;
  int hOld, hNew;
  BfPage **pp;

  assert( pPage->pgno == iOld );

  /* Remove from old hash bucket */
  hOld = iOld % pCache->nHash;
  for(pp = &pCache->apHash[hOld]; *pp; pp = &(*pp)->pNext){
    if( *pp == pPage ){
      *pp = pPage->pNext;
      break;
    }
  }

  /* Add to new hash bucket */
  pPage->pgno = iNew;
  hNew = iNew % pCache->nHash;
  pPage->pNext = pCache->apHash[hNew];
  pCache->apHash[hNew] = pPage;
  if( iNew > pCache->iMaxKey ) pCache->iMaxKey = iNew;
}

/*
** Truncate the cache - remove all pages with pgno >= iLimit.
**
** pager_end_transaction() calls xTruncate on EVERY commit, so the cost of this
** function is paid once per transaction whether or not the database actually
** shrank.  Sweeping the whole hash table here was measured at 50% of all
** cycles on an insert workload, and it made throughput fall 24x as the page
** cache grew from 8 MiB to 128 MiB (36,153 -> 1,512 ops/s with the database on
** tmpfs, i.e. with I/O removed); stock does not have that shape.
**
** So do what pcache1TruncateUnsafe() does upstream, for the same reason:
**
**   - if iLimit is above every page we hold, there is nothing to remove;
**   - if we are only shaving the last few pages off the end, visit just the
**     buckets that can hold a pgno in [iLimit, iMaxKey] -- at most
**     (iMaxKey-iLimit+1) of them -- instead of all nHash;
**   - only when many pages are being dropped is the full sweep worth it.
**
** iMaxKey is maintained by bfCacheAddToHash()/bfCacheRekey() and is an upper
** bound, never an exact maximum: after a truncation it is lowered to
** iLimit-1, and it is not raised again until a page above it is cached.  An
** upper bound is all the range test needs, and being conservative here can
** only cost an unnecessary full sweep, never a missed page.
*/
/*
** D3b known-leaf bitmap (bf_cache.h, BfCache.aLeafBit).  Set grows the map and
** silently does nothing on OOM: a missing bit only costs a leaf read.
*/
void sqlite3BfLeafBitSet(BfCache *pBf, u32 pgno){
  u32 iByte = pgno>>3;
  if( iByte>=pBf->nLeafBit ){
    u32 nNew = pBf->nLeafBit ? pBf->nLeafBit : 1024;
    u8 *aNew;
    while( nNew<=iByte ) nNew *= 2;
    aNew = (u8*)sqlite3_realloc64(pBf->aLeafBit, nNew);
    if( aNew==0 ) return;
    memset(&aNew[pBf->nLeafBit], 0, nNew - pBf->nLeafBit);
    pBf->aLeafBit = aNew;
    pBf->nLeafBit = nNew;
  }
  pBf->aLeafBit[iByte] |= (u8)(1<<(pgno&7));
}
void sqlite3BfLeafBitClear(BfCache *pBf, u32 pgno){
  if( (pgno>>3)<pBf->nLeafBit ) pBf->aLeafBit[pgno>>3] &= (u8)~(1<<(pgno&7));
}
void sqlite3BfLeafBitClearFrom(BfCache *pBf, u32 pgno){
  u32 i;
  if( pBf->nLeafBit==0 ) return;
  if( pgno==0 ){ memset(pBf->aLeafBit, 0, pBf->nLeafBit); return; }
  for(i=pgno; (i&7)!=0 && (i>>3)<pBf->nLeafBit; i++) sqlite3BfLeafBitClear(pBf, i);
  if( (i>>3)<pBf->nLeafBit ){
    memset(&pBf->aLeafBit[i>>3], 0, pBf->nLeafBit - (i>>3));
  }
}
int sqlite3BfLeafBitTest(BfCache *pBf, u32 pgno){
  if( (pgno>>3)>=pBf->nLeafBit ) return 0;
  return (pBf->aLeafBit[pgno>>3]>>(pgno&7)) & 1;
}

static void bfCacheTruncate(sqlite3_pcache *p, unsigned int iLimit){
  BfCacheInt *pCache = (BfCacheInt*)p;
  BfPage *pPage, *pNext;
  unsigned int h, iStop;

  /* Pages at or above iLimit are leaving the snapshot this cache describes
  ** (a shrinking file, or a pager reset after another connection wrote):
  ** whatever they are next, nobody has seen it yet. */
  sqlite3BfLeafBitClearFrom(&pCache->base, iLimit);
#if !defined(SQLITE_BF_NO_FULL_PAGE)
  /* M2: full pages at or above iLimit describe a page this pager no longer
  ** has -- or, on a reset after another connection wrote, no longer trusts. */
  sqlite3BfFullPageDropFrom(&pCache->base, iLimit);
#endif
  if( pCache->nHash==0 ) return;
  if( iLimit > pCache->iMaxKey ) return;      /* nothing at or above iLimit */

  if( (unsigned int)(pCache->iMaxKey - iLimit) < (unsigned int)pCache->nHash ){
    h = iLimit % (unsigned int)pCache->nHash;
    iStop = pCache->iMaxKey % (unsigned int)pCache->nHash;
  }else{
    h = 0;
    iStop = (unsigned int)pCache->nHash - 1;
  }
  for(;;){
    for(pPage = pCache->apHash[h]; pPage; pPage = pNext){
      pNext = pPage->pNext;
      if( pPage->pgno >= iLimit ){
        bfCacheRemoveFromHash(pCache, pPage, 1);
      }
    }
    if( h==iStop ) break;
    h = (h + 1) % (unsigned int)pCache->nHash;
  }
  pCache->iMaxKey = iLimit ? iLimit - 1 : 0;
}

/*
** Destroy a cache.
*/
static void bfCacheDestroy(sqlite3_pcache *p){
  BfCacheInt *pCache = (BfCacheInt*)p;
  int i;
  BfPage *pPage, *pNext;

  /* Free all pages */
  for(i = 0; i < pCache->nHash; i++){
    for(pPage = pCache->apHash[i]; pPage; pPage = pNext){
      pNext = pPage->pNext;
      bfCacheFreePage(pPage);
    }
  }

  BF_ALLOC_TRACE("cache-destroy", pCache, 0);
  sqlite3_free(pCache->base.aDirtyPg);
  pCache->base.aDirtyPg = 0;
  pCache->base.nDirtyPg = 0;
  pCache->base.nDirtyPgAlloc = 0;
  sqlite3_free(pCache->base.aUnlogPg);
  pCache->base.aUnlogPg = 0;
  pCache->base.nUnlogPg = 0;
  pCache->base.nUnlogPgAlloc = 0;
  pCache->base.bUnlogOverflow = 0;
  sqlite3_free(pCache->base.aFlushed);
  pCache->base.aFlushed = 0;
  sqlite3_free(pCache->base.aLeafBit);
  pCache->base.aLeafBit = 0;
  sqlite3_free(pCache->base.pSpareScratch);
  pCache->base.pSpareScratch = 0;
  pCache->base.nLeafBit = 0;
  pCache->base.nFlushed = 0;
  pCache->base.nFlushedAlloc = 0;
#if defined(SQLITE_BF_INSERT_BUFFERING)
  /* Release the group-commit batch.  Anything still in it is DIRTY in its
  ** mini-page, so the close-time flush has already materialised it to base. */
  sqlite3BfBtreeGroupFree(&pCache->base);
#endif
  sqlite3_free(pCache->apHash);
  sqlite3BfCircularBufferDestroy(&pCache->base.cb);
  sqlite3_mutex_free(pCache->base.mutex);
  pCache->base.mutex = 0;
  sqlite3BfMapDestroy(&pCache->base);
  sqlite3_free(pCache);
}

/*
** Shrink the cache by freeing unpinned pages.
*/
static void bfCacheShrink(sqlite3_pcache *p){
  BfCacheInt *pCache = (BfCacheInt*)p;
  BfPage *pPage;

  while( pCache->nRecyclable > 0 ){
    pPage = pCache->lru.pLruPrev;
    if( pPage == &pCache->lru ) break;
    bfCacheRemoveFromHash(pCache, pPage, 1);
  }
}

/*
** Set up the pcache_methods2 interface for Bf-Tree cache.
*/
void sqlite3BfCacheSetMethods(void){
  static const sqlite3_pcache_methods2 bfMethods = {
    1,                     /* iVersion */
    0,                     /* pArg */
    bfCacheInit,           /* xInit */
    bfCacheShutdown,       /* xShutdown */
    bfCacheCreate,         /* xCreate */
    bfCacheCachesize,      /* xCachesize */
    bfCachePagecount,      /* xPagecount */
    bfCacheFetch,          /* xFetch */
    bfCacheUnpin,          /* xUnpin */
    bfCacheRekey,          /* xRekey */
    bfCacheTruncate,       /* xTruncate */
    bfCacheDestroy,        /* xDestroy */
    bfCacheShrink          /* xShrink */
  };
  sqlite3_config(SQLITE_CONFIG_PCACHE2, &bfMethods);
}

/*
** Public wrapper to create a BfCache (P0.3).
** Used for creating per-pager caches.
*/
BfCache *sqlite3BfCacheCreate(int szPage, int szExtra, int bPurgeable){
  return (BfCache*)bfCacheCreate(szPage, szExtra, bPurgeable);
}

/*
** Public wrapper to destroy a BfCache (P0.3).
** Used for cleaning up per-pager caches.
*/
void sqlite3BfCacheDestroy(BfCache *pCache){
  bfCacheDestroy((sqlite3_pcache*)pCache);
}

/*
** High-level cache operations for record-level access.
** These bypass full page operations when possible.
*/

/*
** Copy-on-access second chance (Stage B2; paper 4.1, Figure 14).
**
** The ring evicts in FIFO order, which retains what was promoted most recently
** rather than what is read most often.  Measured at steady state that costs
** 18-25 points of hit rate against the Zipf ideal for the number of records
** actually cached, in every configuration tried.
**
** The reference's answer is not an eviction policy but a REGION: the head-most
** cb_copy_on_access_ratio of the ring (default 10%, PRAGMA bf_copy_on_access
** here).  A page touched inside that region -- i.e. one the head is about to
** reach -- is copied to the tail, so it survives this lap and survives the next
** only if it is touched again.  At ratio 0 that is plain FIFO and at ratio 1 it
** is strict LRU (tree.rs / mini_page_op.rs:151, storage.rs:332).
**
** The copy uses BF_COPY_REFERENCED, the reference's discard_cold_cache
** (leaf_node.rs:1528): the page SHEDS its cold cache records as it relocates.
** That is what makes the move pay for itself, and it is what the earlier
** attempt at this (measured +1.2 pts hit / -3.6% ops, and reverted) was
** missing -- it relocated whole pages and so bought retention with capacity.
**
** DEVIATION, deliberate: we relocate CLEAN mini-pages only, where the
** reference also relocates on insert.  Two reasons, both ours rather than the
** paper's.  (1) evictCallback refuses a dirty mini-page outright, so a dirty
** page is already immune to the head and a second chance buys it nothing.
** (2) The forward merge scan walks a mini-page by INDEX across cursor steps
** (sqlite3BfBtreeMergeNextInsert), and only dirty pages carry the BFOP_INSERT
** records it is walking; shedding records under it would shift those indices.
** Relocating clean pages only keeps that invariant by construction.
*/
void sqlite3BfCacheCopyOnAccess(BfCache *pCache, BfMapEntry *pEntry){
#if !defined(SQLITE_BF_NO_COPY_ON_ACCESS)
  BfMiniPage *pMini, *pNew;
  void *pRaw;
  int nBefore, nAfter;

  if( !pCache || !pEntry ) return;
  if( pCache->cb.copyOnAccessRatio <= 0.0 ) return;
  if( pEntry->locType!=BF_LOC_MINI || !pEntry->pPage ) return;
  if( pCache->bBypassActive || pCache->bMergingActive ) return;

  pMini = (BfMiniPage*)pEntry->pPage;
  if( (void*)pMini == pCache->pEvictProtect ) return;
  if( sqlite3BfMiniPageIsDirty(pMini) ) return;
  if( !sqlite3BfCircularBufferIsCopyOnAccess(&pCache->cb, pMini) ) return;

  pRaw = sqlite3BfCircularBufferAlloc(&pCache->cb, pMini->nodeSize);
  if( !pRaw ) return;      /* no room to move it: let the head have it */
  sqlite3BfCircularBufferMarkReady(pRaw);
  pNew = (BfMiniPage*)pRaw;

  nBefore = sqlite3BfMiniPageCount(pMini);
  if( sqlite3BfMiniPageCopy(pNew, pMini->nodeSize, pMini,
                            BF_COPY_REFERENCED)!=BF_OK ){
    sqlite3BfCircularBufferDealloc(&pCache->cb, pRaw);
    return;                /* leave the original exactly as it was */
  }
  nAfter = sqlite3BfMiniPageCount(pNew);

  /* The copy rebuilds the page flags from the records it carried; STALE is a
  ** property of the PAGE (its base image is out of date), not of any record. */
  pNew->flags |= (u16)(pMini->flags & BF_MINI_F_STALE);

  sqlite3BfCircularBufferDealloc(&pCache->cb, pMini);
  pEntry->pPage = pRaw;
  pCache->nCopyOnAccess++;
  pCache->nCopyOnAccessShed += (u64)(nBefore - nAfter);
#else
  UNUSED_PARAMETER(pCache);
  UNUSED_PARAMETER(pEntry);
#endif
}

/*
** M2 -- full pages (2026-10-02).
**
** ../bf-tree turns a leaf whose mini-page cannot grow any further into a FULL
** PAGE in the same ring: "we are already too large, we need to do whole page
** cache ... it caches the entire gap" (mini_page_op.rs:932-956, reached by
** read-promoted BFOP_CACHE records too, tree.rs:1419).  Scattered hot keys
** therefore stay records and clustered ones become pages, in ONE pool under one
** FIFO/second-chance policy.  Here the leaf IS a SQLite page, so a full page is
** an exact copy of the clean pcache page, and it is served at the pager: a
** pcache miss on that leaf (point read, scan or write) is filled by memcpy
** from the ring instead of a device read (readDbPage, pager.c).
**
** Coherence.  A copy is taken only from a page that is not writeable in the
** current transaction, i.e. equal to what this pager would read.  It is dropped
** whenever that stops being true: the first sqlite3PagerWrite of the page
** (pager_write), a page move, the page being freed (ForgetPage), a shrink or
** pager reset (bfCacheTruncate), ClearCache, and any buffered record write to
** the leaf (sqlite3BfRecordWrite).  The slab is never put on the free list --
** its size is not a size class -- so an unlinked copy is simply an orphan the
** FIFO sweep reclaims.
**
** SQLITE_BF_NO_FULL_PAGE compiles the whole mechanism out.
*/
#if !defined(SQLITE_BF_NO_FULL_PAGE)
int sqlite3BfFullPageCreate(BfCache *pCache, u32 pgno, u32 root, const u8 *aData){
  BfMapEntry *pEntry;
  BfMiniPage *pFull;
  void *pRaw;
  u32 size;

  if( !pCache || pgno<=1 || !aData || pCache->szPage<=0 ) return BF_ERROR;
  if( pCache->bBypassActive || pCache->bMergingActive ) return BF_ERROR;
  size = (u32)sizeof(BfMiniPage) + (u32)pCache->szPage;
  if( size>0xffff ) return BF_ERROR;     /* nodeSize is u16: 64 KiB pages */
  pEntry = sqlite3BfMapLookup(pCache, pgno);
  if( pEntry && pEntry->locType==BF_LOC_MINI && pEntry->pPage
   && sqlite3BfMiniPageIsDirty((BfMiniPage*)pEntry->pPage) ){
    return BF_ERROR;            /* buffered records: never replace them */
  }
  if( pEntry && pEntry->locType==BF_LOC_FULL ) return BF_OK;

  pRaw = sqlite3BfCircularBufferAlloc(&pCache->cb, size);
  if( !pRaw ){
    sqlite3BfCacheEvict(pCache, 16);
    pRaw = sqlite3BfCircularBufferAlloc(&pCache->cb, size);
  }
  if( !pRaw ) return BF_FULL;
  pFull = (BfMiniPage*)pRaw;
  memset(pFull, 0, sizeof(BfMiniPage));
  pFull->nodeSize = (u16)size;
  pFull->flags = BF_MINI_F_FULL;
  pFull->baseDiskOffset = -1;
  pFull->ownerPgno = pgno;
  pFull->rootPgno = root;
  memcpy(BF_FULL_PAGE_DATA(pFull), aData, (size_t)pCache->szPage);
  sqlite3BfCircularBufferMarkReady(pRaw);

  /* Re-read the entry: the evict-and-retry above may have unlinked the clean
  ** mini-page this replaces, or created nothing yet. */
  pEntry = sqlite3BfMapGetOrCreate(pCache, pgno);
  if( !pEntry ) return SQLITE_NOMEM;     /* the slab is an orphan: reclaimed */
  if( pEntry->locType==BF_LOC_MINI && pEntry->pPage ){
    if( sqlite3BfMiniPageIsDirty((BfMiniPage*)pEntry->pPage) ) return BF_ERROR;
    sqlite3BfCircularBufferDealloc(&pCache->cb, pEntry->pPage);
  }
  pEntry->locType = BF_LOC_FULL;
  pEntry->pPage = pRaw;
  if( pgno>pCache->iMaxFullPgno ) pCache->iMaxFullPgno = pgno;
  pCache->nFullCreate++;
  return BF_OK;
}

/* Second chance for a full page, as sqlite3BfCacheCopyOnAccess gives a clean
** mini-page: a whole-slab copy (it has no records to shed).  The old slab is
** left as an orphan rather than freed -- see the coherence note above. */
static void bfFullCopyOnAccess(BfCache *pCache, BfMapEntry *pEntry){
#if !defined(SQLITE_BF_NO_COPY_ON_ACCESS)
  BfMiniPage *pOld = (BfMiniPage*)pEntry->pPage;
  void *pRaw;
  if( pCache->cb.copyOnAccessRatio <= 0.0 ) return;
  if( !sqlite3BfCircularBufferIsCopyOnAccess(&pCache->cb, pOld) ) return;
  pRaw = sqlite3BfCircularBufferAlloc(&pCache->cb, pOld->nodeSize);
  if( !pRaw ) return;
  memcpy(pRaw, pOld, pOld->nodeSize);
  sqlite3BfCircularBufferMarkReady(pRaw);
  pEntry->pPage = pRaw;
  pCache->nCopyOnAccess++;
#else
  UNUSED_PARAMETER(pCache);
  UNUSED_PARAMETER(pEntry);
#endif
}

int sqlite3BfFullPageRead(BfCache *pCache, u32 pgno, void *pBuf, int szPage){
  BfMapEntry *pEntry;
  if( !pCache || pgno<=1 || pgno>pCache->iMaxFullPgno ) return 0;
  if( szPage!=pCache->szPage ) return 0;
  pEntry = sqlite3BfMapLookup(pCache, pgno);
  if( !pEntry || pEntry->locType!=BF_LOC_FULL || !pEntry->pPage ) return 0;
  memcpy(pBuf, BF_FULL_PAGE_DATA(pEntry->pPage), (size_t)szPage);
  pCache->nFullRead++;
  bfFullCopyOnAccess(pCache, pEntry);
  return 1;
}

void sqlite3BfFullPageDrop(BfCache *pCache, u32 pgno){
  BfMapEntry *pEntry;
  if( !pCache || pgno<=1 || pgno>pCache->iMaxFullPgno ) return;
  pEntry = sqlite3BfMapLookup(pCache, pgno);
  if( pEntry && pEntry->locType==BF_LOC_FULL ){
    pEntry->locType = BF_LOC_NULL;     /* slab becomes an orphan */
    pEntry->pPage = 0;
    pCache->nFullDrop++;
  }
}

static int bfFullDropFromCb(void *pCtx, u32 pgno, BfMapEntry *pEntry){
  BfCache *pCache = (BfCache*)pCtx;
  if( pEntry->locType==BF_LOC_FULL && pgno>=pCache->iFullDropLimit ){
    pEntry->locType = BF_LOC_NULL;
    pEntry->pPage = 0;
    pCache->nFullDrop++;
  }
  return 0;
}
/* Drop every full page at or above iLimit (bfCacheTruncate: a shrinking file,
** or a pager reset after another connection wrote). */
void sqlite3BfFullPageDropFrom(BfCache *pCache, u32 iLimit){
  if( pCache->iMaxFullPgno==0 || pCache->iMaxFullPgno<iLimit ) return;
  pCache->iFullDropLimit = iLimit;
  sqlite3BfMapIterate(pCache, bfFullDropFromCb, pCache);
  pCache->iMaxFullPgno = iLimit>0 ? iLimit-1 : 0;
}
#else
int sqlite3BfFullPageCreate(BfCache *p, u32 g, u32 r, const u8 *a){
  UNUSED_PARAMETER(p); UNUSED_PARAMETER(g); UNUSED_PARAMETER(r); UNUSED_PARAMETER(a);
  return BF_ERROR;
}
void sqlite3BfFullPageDrop(BfCache *p, u32 g){ UNUSED_PARAMETER(p); UNUSED_PARAMETER(g); }
#endif /* SQLITE_BF_NO_FULL_PAGE */

/*
** Read a record from the cache.
** First checks mini-page, then falls through to full page.
*/
int sqlite3BfRecordRead(BfCache *pCache, u32 pgno,
    const void *pKey, int nKey, void *pBuf, int *pnBuf){
  BfMapEntry *pEntry;

  pEntry = sqlite3BfMapLookup(pCache, pgno);
  if( !pEntry || pEntry->locType == BF_LOC_NULL ){
    return BF_NOT_FOUND;
  }

  if( pEntry->locType == BF_LOC_MINI && pEntry->pPage ){
    BfMiniPage *pMini = (BfMiniPage*)pEntry->pPage;
    int rc = sqlite3BfMiniPageSearch(pMini, pKey, nKey, pBuf, pnBuf);
    if( rc == BF_OK || rc == BF_DELETED ){
      pCache->nMiniPageHit++;
      /* Served: give the page its second chance if the head is close.  Safe
      ** here and nowhere else in this function -- pMini is dead after this
      ** point, the value has already been copied into the caller's buffer. */
      sqlite3BfCacheCopyOnAccess(pCache, pEntry);
      return rc;
    }
    /* Fall through to check base page */
  }

  pCache->nMiniPageMiss++;
  return BF_NOT_FOUND;  /* Caller should load from disk */
}

/*
** Write a record to the cache.
** Buffers in mini-page when possible.
*/
int sqlite3BfRecordWrite(BfCache *pCache, u32 pgno,
    const void *pKey, int nKey, const void *pVal, int nVal, u8 opType){
  BfMapEntry *pEntry;
  BfMiniPage *pMini;
  int rc;
  int wasDirty;
  int wasUnlogged;
  void *pNew;
  u32 newSize;

  pCache->nInsertGen++;     /* H2 probe memo: a key may become present */
  pEntry = sqlite3BfMapGetOrCreate(pCache, pgno);
  if( !pEntry ){
    BF_ALLOC_TRACE("map-getorcreate-nomem", pCache, (int)pgno);
    return SQLITE_NOMEM;
  }
  if( pEntry->locType==BF_LOC_FULL ){
    /* M2: the whole leaf is already cached; a clean record adds nothing.  A
    ** buffered mutation needs a mini-page, and the copy is still the base
    ** image -- but one map slot holds one location, so the copy goes. */
    if( opType==BFOP_CACHE || opType==BFOP_PHANTOM ) return BF_OK;
    pEntry->locType = BF_LOC_NULL;
    pEntry->pPage = 0;
    pCache->nFullDrop++;
  }

  /* Get or create mini-page */
  if( pEntry->locType == BF_LOC_MINI && pEntry->pPage ){
    pMini = (BfMiniPage*)pEntry->pPage;
  }else{
    /* Allocate new mini-page */
    pNew = sqlite3BfCircularBufferAlloc(&pCache->cb, pCache->aSizeClass[0]);
    if( !pNew ){
      /* Ring full: reclaim clean/orphaned mini-pages from the FIFO head and
      ** retry once.  The sweep refuses dirty mini-pages (evictCallback), so
      ** buffered writes are never dropped; if the head is dirty the sweep
      ** aborts, the retry fails and the caller falls back to the base-page
      ** path.  Safe here: pEntry has no mini-page linked yet, so the sweep
      ** cannot reclaim anything this call still references. */
      sqlite3BfCacheEvict(pCache, 16);
      pNew = sqlite3BfCircularBufferAlloc(&pCache->cb, pCache->aSizeClass[0]);
    }
    if( !pNew ){
      BF_ALLOC_TRACE("cbuf-alloc-null", &pCache->cb, (int)pCache->aSizeClass[0]);
      return BF_FULL;
    }

    sqlite3BfMiniPageInit((BfMiniPage*)pNew, pCache->aSizeClass[0], pEntry->diskOffset);
    sqlite3BfCircularBufferMarkReady(pNew);

    pEntry->locType = BF_LOC_MINI;
    pEntry->pPage = pNew;
    pMini = (BfMiniPage*)pNew;
    /* Record the back-pointers so the eviction sweep can resolve ownership by
    ** a direct map lookup.  Today the map is keyed per table, so owner == root
    ** == pgno; Phase 1.3 will pass the table root separately once owner becomes
    ** the hot leaf.  The size-upgrade path (below) carries these forward via
    ** sqlite3BfMiniPageCopy, so they need only be set here at creation. */
    pMini->ownerPgno = pgno;
    pMini->rootPgno = pgno;
  }

  /* Try to insert into mini-page */
  wasDirty = (pMini->flags & BF_MINI_F_DIRTY)!=0;
  wasUnlogged = (pMini->flags & BF_MINI_F_UNLOGGED)!=0;
  rc = sqlite3BfMiniPageInsert(pMini, pKey, nKey, pVal, nVal, opType);

  if( rc == BF_OK ){
    pCache->nDirty++;
    if( !wasDirty && (pMini->flags & BF_MINI_F_DIRTY)!=0 ){
      sqlite3BfDirtyListAdd(pCache, pgno);   /* clean -> dirty transition */
    }
    if( !wasUnlogged && (pMini->flags & BF_MINI_F_UNLOGGED)!=0 ){
      sqlite3BfUnlogListAdd(pCache, pgno);   /* now has something to log */
    }
    return BF_OK;
  }

  /* Mini-page full - upgrade to the smallest size class that actually FITS
  ** this record alongside the live ones.  Stepping a single class up (the old
  ** behaviour) left every record larger than 96 bytes unbufferable, because
  ** one step from the 64-byte initial allocation reaches only 128. */
  newSize = sqlite3BfMiniPageSizeClassFor(pMini, nKey, nVal,
                                          pCache->aSizeClass);
#if !defined(SQLITE_BF_NO_FULL_PAGE)
  /* M2: the reference's trigger.  A clean mini-page that would need a slab as
  ** large as the page itself (or cannot grow at all) caches the whole leaf
  ** instead -- a page then holds every row of the leaf for the bytes the
  ** mini-page would spend on a fraction of them.  The caller holds the page
  ** (sqlite3BfBtreePromoteRecord) and makes the copy. */
  if( (opType==BFOP_CACHE || opType==BFOP_PHANTOM) && !wasDirty
   && pCache->szPage>0
   && (newSize==0 || newSize >= (u32)pCache->szPage) ){
    return BF_WANT_FULL;
  }
#endif
  if( newSize > 0 && newSize <= BF_MAX_MINI_PAGE ){
    pNew = sqlite3BfCircularBufferAlloc(&pCache->cb, newSize);
    if( !pNew ){
      /* Ring full: reclaim from the FIFO head and retry, exactly as the
      ** create-a-mini-page path above does.  Without this the upgrade simply
      ** fails and the record is not cached -- and because nothing else ever
      ** calls sqlite3BfCacheEvict, a full ring meant a leaf could never grow
      ** past the size it happened to reach, so the cache froze with whatever
      ** it had admitted early.  Measured at saturation (16 MiB ring, 4M rows,
      ** zipf 0.99): 46.4% hit rate where the ideal for that many cached records
      ** is 71.5%.  The sweep refuses dirty mini-pages, so buffered writes are
      ** never dropped. */
      pCache->pEvictProtect = pMini;
      sqlite3BfCacheEvict(pCache, 16);
      pCache->pEvictProtect = 0;
      pNew = sqlite3BfCircularBufferAlloc(&pCache->cb, newSize);
    }
    if( pNew ){
      sqlite3BfCircularBufferMarkReady(pNew);
      /* Shed cold cache records as we grow, exactly as the reference does at
      ** this same point -- mini_page_op.rs:744 upgrades with
      ** copy_initialize_to(..., discard_cold_cache = true).
      **
      ** This call used to pass BF_COPY_ALL, and the reason given was sound at
      ** the time: "this mini-page may hold buffered dirty writes that have
      ** never been read back, so their REF bit is irrelevant; dropping them
      ** would silently lose committed data."  BF_COPY_REFERENCED then meant
      ** "keep records with the REF bit", which would indeed have dropped
      ** them.  It now means the reference's discard_cold_cache
      ** (sqlite3BfKvIsColdCache): a dirty record is never cold whatever its
      ** REF bit says, so the hazard that comment describes cannot occur.
      **
      ** This is the copy that MATTERS for capacity.  Upgrades outnumber
      ** copy-on-access relocations by orders of magnitude -- every mini-page
      ** that outgrows its size class comes through here -- so it is where the
      ** paper's shedding actually buys ring space back.
      **
      ** DEFAULT OFF, against the reference, pending the end-of-B campaign.
      ** Tripwire A/B (30k rows, 256 KiB ring, promotion 100, NOT quotable):
      ** shedding bought +13.6% cached_records and -11.5% B/record, and cost
      ** NINE POINTS of hit rate (28.8% -> 19.7%).  The records it drops were
      ** genuinely never read -- carrying the REF bit across the copy was built
      ** and measured and changed the numbers not at all -- they were simply
      ** read LATER, after the upgrade that shed them.  So "not read since it
      ** entered this page" is a poor coldness proxy whenever pages are copied
      ** often relative to the reuse distance, which is our situation and may
      ** not be the reference's.
      **
      ** A nine-point move is over the "stop" line for the tripwire tier, so
      ** this does not ship on by default on faith.  Build with
      ** -DSQLITE_BF_UPGRADE_SHED to enable it; the campaign decides. */
      rc = sqlite3BfMiniPageCopy((BfMiniPage*)pNew, newSize, pMini,
#if defined(SQLITE_BF_UPGRADE_SHED)
                                 BF_COPY_REFERENCED);
#else
                                 BF_COPY_ALL);
#endif
      if( rc==BF_OK ){
        int nShed = sqlite3BfMiniPageCount(pMini)
                      - sqlite3BfMiniPageCount((BfMiniPage*)pNew);
        if( nShed>0 ) pCache->nUpgradeShed += (u64)nShed;
      }
      if( rc != BF_OK ){
        sqlite3BfCircularBufferDealloc(&pCache->cb, pNew);
        if( rc == SQLITE_NOMEM ) return SQLITE_NOMEM;
        return rc;
      }

      sqlite3BfCircularBufferDealloc(&pCache->cb, pMini);

      /* Restore locType as well as pPage.  The evict-and-retry above can have
      ** unlinked this entry (locType = BF_LOC_NULL); re-pointing pPage alone
      ** then leaves a live mini-page that every read skips at its locType
      ** check.  pEvictProtect now stops that sweep, so this is belt and braces
      ** — but the two must be written together or the invariant is only true by
      ** accident. */
      pEntry->locType = BF_LOC_MINI;
      pEntry->pPage = pNew;
      pMini = (BfMiniPage*)pNew;
      pCache->nUpgrades++;

      rc = sqlite3BfMiniPageInsert(pMini, pKey, nKey, pVal, nVal, opType);
      if( rc == BF_OK ){
        pCache->nDirty++;
        if( !wasDirty && (pMini->flags & BF_MINI_F_DIRTY)!=0 ){
          sqlite3BfDirtyListAdd(pCache, pgno);
        }
        if( !wasUnlogged && (pMini->flags & BF_MINI_F_UNLOGGED)!=0 ){
          sqlite3BfUnlogListAdd(pCache, pgno);
        }
        return BF_OK;
      }
    }
  }

  /* Mini-page is at its max size class (or the ring refused a bigger block).
  ** Before giving up on a DIRTY record, reclaim the space held by this
  ** mini-page's pure-cache records: BFOP_CACHE duplicates a base cell and
  ** BFOP_PHANTOM marks a confirmed-absent key, so dropping either costs at most
  ** a later cache miss.  This matters a lot in practice — every insert's
  ** uniqueness probe leaves a PHANTOM behind, so without compaction a leaf's
  ** mini-page fills with clean junk and every subsequent buffered insert falls
  ** back to a base-page write (which then drags the whole table's dirty
  ** mini-pages to base with it). */
#if !defined(SQLITE_BF_NO_MINIPAGE_COMPACT)
  if( opType==BFOP_INSERT || opType==BFOP_DELETE ){
    u16 curSize = pMini->nodeSize;
    if( sqlite3BfMiniPageDirtyCount(pMini) < sqlite3BfMiniPageCount(pMini) ){
      pNew = sqlite3BfCircularBufferAlloc(&pCache->cb, curSize);
      if( !pNew ){
        pCache->pEvictProtect = pMini;     /* same reclaim-and-retry */
        sqlite3BfCacheEvict(pCache, 16);
        pCache->pEvictProtect = 0;
        pNew = sqlite3BfCircularBufferAlloc(&pCache->cb, curSize);
      }
      if( pNew ){
        sqlite3BfCircularBufferMarkReady(pNew);
        rc = sqlite3BfMiniPageCopy((BfMiniPage*)pNew, curSize, pMini,
                                   BF_COPY_DIRTY);
        if( rc==BF_OK ){
          sqlite3BfCircularBufferDealloc(&pCache->cb, pMini);
          pEntry->locType = BF_LOC_MINI;   /* see the upgrade path above */
          pEntry->pPage = pNew;
          pMini = (BfMiniPage*)pNew;
          pCache->nCompactions++;
          rc = sqlite3BfMiniPageInsert(pMini, pKey, nKey, pVal, nVal, opType);
          if( rc==BF_OK ){
            pCache->nDirty++;
            if( !wasDirty && (pMini->flags & BF_MINI_F_DIRTY)!=0 ){
              sqlite3BfDirtyListAdd(pCache, pgno);
            }
            if( !wasUnlogged && (pMini->flags & BF_MINI_F_UNLOGGED)!=0 ){
              sqlite3BfUnlogListAdd(pCache, pgno);
            }
            return BF_OK;
          }
        }else{
          /* Compaction failed: keep the original mini-page intact. */
          sqlite3BfCircularBufferDealloc(&pCache->cb, pNew);
          if( rc==SQLITE_NOMEM ) return SQLITE_NOMEM;
        }
      }
    }
  }
#endif /* !SQLITE_BF_NO_MINIPAGE_COMPACT */

  /* Still no room — caller must use the full base-page path for this record. */
  return BF_FULL;
}

/*
** Phase 2 (WAL) recovery: repopulate the record cache from the WAL's rebuilt
** pgno->ops index.  After a crash, walIndexRecover reconstructs pWalIdx by
** scanning the WAL's record frames; this replays those ops (in log order) back
** into the mini-page cache so the Phase-1 read hook surfaces them to queries —
** the WAL is the durable source that rehydrates the RAM cache.
**
** Records are written back as DIRTY (BFOP_INSERT/BFOP_DELETE): they are durable
** in the WAL but NOT yet in the base B-tree (checkpoint applies them lazily), so
** they must still be flushed to base at checkpoint/eviction.  They are left
** UNLOGGED (the logged flag stays clear): a subsequent commit will re-log them
** once — harmless because replay is idempotent and order-preserving — which is
** simpler than reconstructing the exactly-once marks across a crash.
**
** WAL format v2 persists both the target leaf and the owning table root.  The
** leaf remains the cache-map key; rootPgno is restored onto the mini-page so a
** checkpoint can open its replay cursor on the real table root even if no query
** has descended the table since recovery.  Treating the leaf as the root is not
** a harmless fallback: once replay splits that leaf, it creates a second tree
** beneath a child of the real root and leaves the B-tree structurally corrupt.
**
** Returns SQLITE_OK, or SQLITE_NOMEM if a write ran out of memory.
*/
typedef struct BfReplayCtx BfReplayCtx;
struct BfReplayCtx {
  BfCache    *pCache;
  BfWalIndex *pWalIdx;
  u32         mxFrame;      /* committed end of the log */
  int         bKeepBeyond;  /* replay ops past mxFrame too (rollback) */
  int         rc;       /* first error, sticky */
};

/*
** Ordering.  The index keeps each leaf's ops in log order, but the base tree a
** replay writes them over is the NEWEST image of every page.  An op a flush has
** already put into base pages must therefore not be replayed: measured before
** this rule existed, an UPDATE and a DELETE that reached base through the page
** path were undone by the next checkpoint (the stale INSERT overwrote the
** update and resurrected the deleted row), and count(*) double-counted the
** replayed rows in between.
**
** The flush says so explicitly: the commit that carries its page images logs a
** BFWAL_OP_CLEAR for every leaf whose records it applied, and replay starts
** each leaf after its last CLEAR.  A first attempt inferred the same thing from
** page images instead ("a newer image of the op's leaf contains the op"), and
** the crash oracle broke it within a few hundred runs: a flush writes a leaf's
** records wherever their keys live NOW, and after a split that is another leaf,
** whose image says nothing about the first.  An op no CLEAR covers was never
** applied to base, and no page-path write can have touched its key since --
** such a write needs the row in base, i.e. a flush first -- so replaying it over
** the newest images is correct.
**
** Ops carried by frames past mxFrame belong to no committed log: at recovery
** they are the torn tail of a crashed commit and are skipped; at rollback they
** are group-commit batches of EARLIER, committed transactions that a spill
** wrote inside the rolled-back one, and are kept (bKeepBeyond).
*/
static int bfReplayOnePage(void *pCtx, u32 pgno){
  BfReplayCtx *ctx = (BfReplayCtx*)pCtx;
  int n = sqlite3BfWalIndexPageCount(ctx->pWalIdx, pgno);
  int i, iStart = 0;
  /* Start after the last CLEAR that is part of the log being replayed. */
  for(i=n-1; i>=0; i--){
    BfWalRec rec;
    if( sqlite3BfWalIndexGet(ctx->pWalIdx, pgno, i, &rec)!=BFWAL_OK ) break;
    if( rec.op!=BFWAL_OP_CLEAR ) continue;
    /* A CLEAR past the log's end is never honoured, even when other ops there
    ** are kept: CLEARs are written only by a commit, so one past mxFrame
    ** belongs to a commit that did not complete, whose flush was undone. */
    if( rec.iFrame>ctx->mxFrame ) continue;
    iStart = i+1;
    break;
  }
  ctx->pCache->nReplaySuperseded += (u64)iStart;
  for(i=iStart; i<n; i++){
    BfWalRec rec;
    u8 op;
    int wr;
    if( sqlite3BfWalIndexGet(ctx->pWalIdx, pgno, i, &rec)!=BFWAL_OK ) break;
    if( rec.iFrame>ctx->mxFrame && !ctx->bKeepBeyond ){
      ctx->pCache->nReplayTorn++;
      continue;
    }
    if( rec.op==BFWAL_OP_CLEAR ) continue;  /* a torn-tail CLEAR, skipped above */
    ctx->pCache->nReplayApplied++;
    op = (rec.op==BFWAL_OP_DELETE) ? BFOP_DELETE : BFOP_INSERT;
    wr = sqlite3BfRecordWrite(ctx->pCache, pgno, rec.pKey, (int)rec.nKey,
                              rec.pVal, (int)rec.nVal, op);
    /* A normal write that gets BF_FULL falls back to the base page; replay
    ** has no fallback, so it must not give up while the ring can still yield
    ** space.  sqlite3BfRecordWrite's own reclaim is one 16-slab sweep, and
    ** right after a rollback's ClearCache the FIFO head is a run of small
    ** orphans: 16 of them did not free room for one ~1.4 KB record, and the
    ** committed row was dropped (gen_update_stress seed 11, ring variant,
    ** 2026-10-03).  Sweep until it fits or the sweep stops progressing (empty
    ** ring, or a dirty slab at the head). */
    while( wr==BF_FULL && sqlite3BfCacheEvict(ctx->pCache, 64)>0 ){
      wr = sqlite3BfRecordWrite(ctx->pCache, pgno, rec.pKey, (int)rec.nKey,
                                rec.pVal, (int)rec.nVal, op);
    }
    if( wr==BF_OK ){
      BfMapEntry *pEntry = sqlite3BfMapLookup(ctx->pCache, pgno);
      if( pEntry && pEntry->locType==BF_LOC_MINI && pEntry->pPage ){
        ((BfMiniPage*)pEntry->pPage)->rootPgno = rec.rootPgno;
      }
    }
    if( wr==SQLITE_NOMEM ){
      ctx->rc = SQLITE_NOMEM;
      return BFWAL_NOMEM;   /* stop the walk */
    }
    /* BF_FULL (mini-page maxed) used to be "tolerated" on the grounds that the
    ** record stays durable in the WAL and is re-applied at the next checkpoint
    ** from the base path.  Nothing does that: checkpoint materialises the CACHE
    ** and never reads record frames.  A refused op is a lost committed record,
    ** so count it where it cannot be missed (bf_cache_stats replay_dropped). */
    if( wr!=BF_OK ) ctx->pCache->nReplayDropped++;
  }
  return BFWAL_OK;
}

int sqlite3BfCacheReplayWal(BfCache *pCache, BfWalIndex *pWalIdx,
    u32 mxFrame, int bKeepBeyond){
  BfReplayCtx ctx;
  if( pCache==0 || pWalIdx==0 ) return SQLITE_OK;
  ctx.pCache  = pCache;
  ctx.pWalIdx = pWalIdx;
  ctx.mxFrame = mxFrame;
  ctx.bKeepBeyond = bKeepBeyond;
  ctx.rc      = SQLITE_OK;
  sqlite3BfWalIndexForEachPage(pWalIdx, bfReplayOnePage, &ctx);
  if( ctx.rc==SQLITE_OK ){
    /* Arm the pre-mutation / pre-scan / commit flush for the replayed rows. */
    pCache->bDirtyInserts = 1;
  }
  return ctx.rc;
}

/*
** Callback function to apply dirty records to the base page during merge.
** This function is called for each record in the mini-page during merge.
**
** Parameters:
**   pCtx: BfApplyContext containing Pager and page number
**   pKey: Key bytes
**   nKey: Key length
**   pVal: Value bytes
**   nVal: Value length
**   opType: Operation type (BFOP_INSERT or BFOP_DELETE)
**
** Return: BF_OK on success, error code otherwise
*/
static int bfApplyDirtyRecordCallback(void *pCtx, const u8 *pKey, int nKey, 
                                      const u8 *pVal, int nVal, u8 opType){
  BfApplyContext *pApplyCtx = (BfApplyContext*)pCtx;
  Pager *pPager = pApplyCtx->pPager;
  u32 pgno = pApplyCtx->pgno;
  int rc;

  /* Only process dirty records (INSERT or DELETE operations) */
  if( opType == BFOP_INSERT ){
    /* Apply the insert to the base page */
    rc = sqlite3BfBtreeApplyInsert(pPager, pgno, pKey, nKey, pVal, nVal);
    if( rc != SQLITE_OK ){
      return rc;
    }
  }else if( opType == BFOP_DELETE ){
    /* Apply the delete to the base page */
    rc = sqlite3BfBtreeApplyDelete(pPager, pgno, pKey, nKey);
    if( rc != SQLITE_OK ){
      return rc;
    }
  }
  /* Other operation types (BFOP_CACHE, BFOP_PHANTOM) are not dirty records */
  
  return BF_OK;
}

/*
** Evict pages to free memory.
** Called by circular buffer when space is needed.
**
** Invoked for each allocation the FIFO sweep wants to reclaim.  Three cases:
**
**   1. Orphaned (no mapping entry references it): the mini-page was unlinked
**      by ClearCache after a rollback or superseded by a size upgrade.  Its
**      contents are unreachable, dirty or not — reclaim freely.
**
**   2. Mapped and clean: every record is BFOP_CACHE/BFOP_PHANTOM, i.e. pure
**      read-cache over data the base pages already hold.  Unlink the mapping
**      entry (so no dangling pointer survives the reclaim) and evict.
**
**   3. Mapped and dirty: holds buffered BFOP_INSERT/BFOP_DELETE records that
**      have NOT been applied to the base pages.  Eviction cannot apply them
**      here — the flush needs a Btree handle and a re-entrancy-safe call
**      point (see bfFlushOneMiniPage) — so REFUSE.  The sweep aborts and the
**      writer falls back to the base-page path; the buffered records stay
**      dirty and are applied by the next scan-prep or commit-time flush.
*/
static int evictCallback(void *pCtx, void *ptr){
  BfCache *pCache = (BfCache*)pCtx;
  BfMiniPage *pMini = (BfMiniPage*)ptr;
  BfMapEntry *pEntry;

  if( !pMini ) return BF_OK;

  /* Resolve ownership directly from the mini-page's back-pointer instead of a
  ** linear map scan (the map holds hundreds of entries once keyed per leaf).
  ** The entry may since have been re-pointed at a newer slab (size upgrade) or
  ** unlinked (ClearCache); only the entry that still points AT this exact slab
  ** owns it.  Any other outcome means the slab is orphaned. */
  /* Case 0: the slab a caller is mid-copy of.  sqlite3BfRecordWrite's upgrade
  ** and compaction paths set pEvictProtect before their evict-and-retry
  ** precisely so this sweep cannot take the page they are copying FROM — and
  ** until 2026-09-21 this function never looked at it, so the protection did
  ** nothing.  The consequence was silent data loss, not just a wasted copy: the
  ** sweep unlinks a CLEAN mini-page by setting locType = BF_LOC_NULL, the
  ** caller then re-points pEntry->pPage at its new slab WITHOUT restoring
  ** locType, and every later sqlite3BfRecordRead on that pgno returns
  ** BF_NOT_FOUND at its locType check — the record is present and invisible.
  **
  ** Reproduced by a cross-table differential against stock (12 tables, 256 KiB
  ** ring): exactly one INSERT lost, and sqlite3BfRecordRead returning
  ** BF_NOT_FOUND one instruction after sqlite3BfRecordWrite returned BF_OK for
  ** the same leaf and key.  It needs a CLEAN mini-page (a dirty one is refused
  ** below) that is being written into while the ring is full, which is why the
  ** single-table oracles never produced it. */
  if( (void*)pMini == pCache->pEvictProtect ) return BF_ERROR;

  pEntry = sqlite3BfMapLookup(pCache, pMini->ownerPgno);
  if( pEntry && pEntry->locType==BF_LOC_FULL && pEntry->pPage==pMini ){
    pEntry->locType = BF_LOC_NULL;   /* M2: a full page is always clean */
    pEntry->pPage = 0;
    pCache->nFullEvict++;
    return BF_OK;
  }
  if( pEntry == 0 || pEntry->locType != BF_LOC_MINI || pEntry->pPage != pMini ){
    return BF_OK;            /* Case 1: orphaned */
  }
  if( sqlite3BfMiniPageIsDirty(pMini) ){
    /* Case 3: dirty — refuse, never drop data.  Record WHICH leaf stalled the
    ** sweep so the btree layer can flush it and let the next sweep past; see
    ** sqlite3BfBtreeRelieveEvictStall().  Recording it here is free and cannot
    ** affect this call's outcome — we still refuse. */
    pCache->pgnoEvictStall = pMini->ownerPgno;
    pCache->nEvictStallSeen++;
    return BF_ERROR;
  }

  /* Case 2: clean — unlink, then allow the reclaim */
  pEntry->locType = BF_LOC_NULL;
  pEntry->pPage = 0;
  return BF_OK;
}


int sqlite3BfCacheEvict(BfCache *pCache, int nTarget){
  int nEvicted = 0;

  /* Scope pgnoEvictStall to THIS sweep (M1).  Leaving it set across calls made
  ** it sticky, and the counters showed the cost: a stall recorded by one sweep
  ** was consumed by a later, unrelated refusal -- a mini-page already at the
  ** maximum size class, which no flush can help -- so 52 flushes bought 4
  ** rescued inserts.  Cleared here, a non-zero stall afterwards means the sweep
  ** that just ran was genuinely blocked by a dirty slab. */
  pCache->pgnoEvictStall = 0;

  nEvicted = sqlite3BfCircularBufferEvictN(&pCache->cb, nTarget,
      evictCallback, pCache);

  pCache->nEvictions += nEvicted;
  return nEvicted;
}

/*
** Per-pager cache management (P0.3).
** Gets a BF-Tree cache for a specific pager (created via sqlite3PagerOpenBfCache).
** Implemented in pager.c where the Pager struct is fully defined.
*/

#endif /* !defined(SQLITE_OMIT_BF_CACHE) */
