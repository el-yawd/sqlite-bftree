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
static BfCache *bfGlobalCache;

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
** Resize the hash table.
*/
static int bfCacheResizeHash(BfCacheInt *pCache){
  BfPage **apNew;
  int nNew;
  int i;
  BfPage *pPage, *pNext;

  nNew = pCache->nHash ? pCache->nHash * 2 : 256;
  apNew = (BfPage**)sqlite3MallocZero(sizeof(BfPage*) * nNew);
  if( !apNew ) return SQLITE_NOMEM;

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

  /* Initialize circular buffer */
  rc = sqlite3BfCircularBufferInit(&pCache->base.cb, sqlite3BfCacheBufferSize());
  if( rc != SQLITE_OK ){
    BF_ALLOC_TRACE("cache-create-fail", pCache, (int)sz);
    sqlite3_free(pCache);
    return 0;
  }

  /* Initialize mapping table */
  rc = sqlite3BfMapInit(&pCache->base);
  if( rc != SQLITE_OK ){
    BF_ALLOC_TRACE("cache-create-fail", pCache, (int)sz);
    sqlite3BfCircularBufferDestroy(&pCache->base.cb);
    sqlite3_free(pCache);
    return 0;
  }

  /* Initialize size classes */
  {
    int i;
    u32 size = BF_MIN_MINI_PAGE;
    for(i = BF_SIZE_CLASS_COUNT - 1; i >= 0; i--){
      pCache->base.aSizeClass[i] = size;
      size *= 2;
    }
  }

  /* Initialize hash table */
  rc = bfCacheResizeHash(pCache);
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
      pPage->pCache->nMiniPageHit++;
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
      /* Reuse the page structure */
      pPage = pRecycle;
      memset(pPage->page.pBuf, 0, pCache->base.szPage);
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

  pPage->pCache->nMiniPageMiss++;

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
}

/*
** Truncate the cache - remove all pages with pgno > iLimit.
*/
static void bfCacheTruncate(sqlite3_pcache *p, unsigned int iLimit){
  BfCacheInt *pCache = (BfCacheInt*)p;
  int i;
  BfPage *pPage, *pNext;

  for(i = 0; i < pCache->nHash; i++){
    for(pPage = pCache->apHash[i]; pPage; pPage = pNext){
      pNext = pPage->pNext;
      if( pPage->pgno >= iLimit ){
        bfCacheRemoveFromHash(pCache, pPage, 1);
      }
    }
  }
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
#if defined(SQLITE_BF_INSERT_BUFFERING)
  /* Release the group-commit batch.  Anything still in it is DIRTY in its
  ** mini-page, so the close-time flush has already materialised it to base. */
  sqlite3BfBtreeGroupFree(&pCache->base);
#endif
  sqlite3_free(pCache->apHash);
  sqlite3BfCircularBufferDestroy(&pCache->base.cb);
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
** Initialize the Bf-Tree cache subsystem.
*/
int sqlite3BfCacheInit(void){
  return bfCacheInit(0);
}

/*
** Shutdown the Bf-Tree cache subsystem.
*/
void sqlite3BfCacheShutdown(void){
  bfCacheShutdown(0);
}

/*
** Return the lazily created runtime Bf-Tree cache used by btree hooks.
**
** This path does not depend on runtime sqlite3_config(SQLITE_CONFIG_PCACHE2)
** reconfiguration, which is disallowed after initialization.
*/
BfCache *sqlite3BfGetGlobalCache(int szPage){
  BfCache *pRet;
  if( !sqlite3BfCacheEnabled() ) return 0;
  if( szPage<=0 ) szPage = 4096;
#if SQLITE_THREADSAFE
  sqlite3_mutex_enter(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MAIN));
#endif
  if( bfGlobalCache==0 ){
    bfGlobalCache = (BfCache*)bfCacheCreate(szPage, 0, 1);
  }
  pRet = bfGlobalCache;
#if SQLITE_THREADSAFE
  sqlite3_mutex_leave(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MAIN));
#endif
  return pRet;
}

void sqlite3BfResetGlobalCache(void){
#if SQLITE_THREADSAFE
  sqlite3_mutex_enter(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MAIN));
#endif
  if( bfGlobalCache ){
    bfCacheDestroy((sqlite3_pcache*)bfGlobalCache);
    bfGlobalCache = 0;
  }
#if SQLITE_THREADSAFE
  sqlite3_mutex_leave(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MAIN));
#endif
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
    if( rc == BF_OK ){
      pCache->nMiniPageHit++;
      return BF_OK;
    }else if( rc == BF_DELETED ){
      pCache->nMiniPageHit++;
      return BF_DELETED;
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
  void *pNew;
  u32 newSize;

  pEntry = sqlite3BfMapGetOrCreate(pCache, pgno);
  if( !pEntry ){
    BF_ALLOC_TRACE("map-getorcreate-nomem", pCache, (int)pgno);
    return SQLITE_NOMEM;
  }

  /* Get or create mini-page */
  if( pEntry->locType == BF_LOC_MINI && pEntry->pPage ){
    pMini = (BfMiniPage*)pEntry->pPage;
  }else{
    /* Allocate new mini-page */
    pNew = sqlite3BfCircularBufferAlloc(&pCache->cb, BF_MIN_MINI_PAGE);
    if( !pNew ){
      /* Ring full: reclaim clean/orphaned mini-pages from the FIFO head and
      ** retry once.  The sweep refuses dirty mini-pages (evictCallback), so
      ** buffered writes are never dropped; if the head is dirty the sweep
      ** aborts, the retry fails and the caller falls back to the base-page
      ** path.  Safe here: pEntry has no mini-page linked yet, so the sweep
      ** cannot reclaim anything this call still references. */
      sqlite3BfCacheEvict(pCache, 16);
      pNew = sqlite3BfCircularBufferAlloc(&pCache->cb, BF_MIN_MINI_PAGE);
    }
    if( !pNew ){
      BF_ALLOC_TRACE("cbuf-alloc-null", &pCache->cb, BF_MIN_MINI_PAGE);
      return BF_FULL;
    }

    sqlite3BfMiniPageInit((BfMiniPage*)pNew, BF_MIN_MINI_PAGE, pEntry->diskOffset);
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
  rc = sqlite3BfMiniPageInsert(pMini, pKey, nKey, pVal, nVal, opType);

  if( rc == BF_OK ){
    pCache->nDirty++;
    if( !wasDirty && (pMini->flags & BF_MINI_F_DIRTY)!=0 ){
      sqlite3BfDirtyListAdd(pCache, pgno);   /* clean -> dirty transition */
    }
    return BF_OK;
  }

  /* Mini-page full - try to upgrade to the next size class */
  newSize = sqlite3BfMiniPageNextSizeClass(pMini, pCache->aSizeClass);
  if( newSize > 0 && newSize <= BF_MAX_MINI_PAGE ){
    pNew = sqlite3BfCircularBufferAlloc(&pCache->cb, newSize);
    if( pNew ){
      sqlite3BfCircularBufferMarkReady(pNew);
      /* Copy ALL records, not just referenced ones: this mini-page may hold
      ** buffered dirty writes (BFOP_INSERT/BFOP_DELETE) that have never been
      ** read back, so their REF bit is irrelevant.  Dropping them here would
      ** silently lose committed data on the next flush. */
      rc = sqlite3BfMiniPageCopy((BfMiniPage*)pNew, newSize, pMini,
                                 BF_COPY_ALL);
      if( rc != BF_OK ){
        sqlite3BfCircularBufferDealloc(&pCache->cb, pNew);
        if( rc == SQLITE_NOMEM ) return SQLITE_NOMEM;
        return rc;
      }

      sqlite3BfCircularBufferDealloc(&pCache->cb, pMini);

      pEntry->pPage = pNew;
      pMini = (BfMiniPage*)pNew;
      pCache->nUpgrades++;

      rc = sqlite3BfMiniPageInsert(pMini, pKey, nKey, pVal, nVal, opType);
      if( rc == BF_OK ){
        pCache->nDirty++;
        if( !wasDirty && (pMini->flags & BF_MINI_F_DIRTY)!=0 ){
          sqlite3BfDirtyListAdd(pCache, pgno);
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
      if( pNew ){
        sqlite3BfCircularBufferMarkReady(pNew);
        rc = sqlite3BfMiniPageCopy((BfMiniPage*)pNew, curSize, pMini,
                                   BF_COPY_DIRTY);
        if( rc==BF_OK ){
          sqlite3BfCircularBufferDealloc(&pCache->cb, pMini);
          pEntry->pPage = pNew;
          pMini = (BfMiniPage*)pNew;
          pCache->nCompactions++;
          rc = sqlite3BfMiniPageInsert(pMini, pKey, nKey, pVal, nVal, opType);
          if( rc==BF_OK ){
            pCache->nDirty++;
            if( !wasDirty && (pMini->flags & BF_MINI_F_DIRTY)!=0 ){
              sqlite3BfDirtyListAdd(pCache, pgno);
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
** KNOWN GAP (leaf->root): a mini-page created here has no rootPgno (the WAL
** stores only the leaf pgno).  Reads work (they key by leaf), but a checkpoint
** that must flush a purely-recovered record to base needs the owning table root
** — which is re-established when a query descends that table (bfTagLeafRoot in
** the btree hooks).  Post-recovery checkpoint of never-touched pages is future
** work (see [[phase2-progress]] task 6).
**
** Returns SQLITE_OK, or SQLITE_NOMEM if a write ran out of memory.
*/
typedef struct BfReplayCtx BfReplayCtx;
struct BfReplayCtx {
  BfCache    *pCache;
  BfWalIndex *pWalIdx;
  int         rc;       /* first error, sticky */
};

static int bfReplayOnePage(void *pCtx, u32 pgno){
  BfReplayCtx *ctx = (BfReplayCtx*)pCtx;
  int n = sqlite3BfWalIndexPageCount(ctx->pWalIdx, pgno);
  int i;
  for(i=0; i<n; i++){
    BfWalRec rec;
    u8 op;
    int wr;
    if( sqlite3BfWalIndexGet(ctx->pWalIdx, pgno, i, &rec)!=BFWAL_OK ) break;
    op = (rec.op==BFWAL_OP_DELETE) ? BFOP_DELETE : BFOP_INSERT;
    wr = sqlite3BfRecordWrite(ctx->pCache, pgno, rec.pKey, (int)rec.nKey,
                              rec.pVal, (int)rec.nVal, op);
    if( wr==SQLITE_NOMEM ){
      ctx->rc = SQLITE_NOMEM;
      return BFWAL_NOMEM;   /* stop the walk */
    }
    /* BF_FULL (mini-page maxed) is tolerated: the record stays durable in the
    ** WAL and is re-applied at the next checkpoint from the base path. */
  }
  return BFWAL_OK;
}

int sqlite3BfCacheReplayWal(BfCache *pCache, BfWalIndex *pWalIdx){
  BfReplayCtx ctx;
  if( pCache==0 || pWalIdx==0 ) return SQLITE_OK;
  ctx.pCache  = pCache;
  ctx.pWalIdx = pWalIdx;
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
** Merge a mini-page to its base page (internal implementation).
**
** At minimum this compacts the mini-page and converts dirty records into a
** clean state so scans and eviction can proceed without leaving stale deltas
** behind.
*/
static int bfCacheMergeInternal(BfCache *pCache, u32 pgno, Pager *pPager){
  BfMapEntry *pEntry;
  BfMiniPage *pMini;
  int nDirty;
  int rc;

  UNUSED_PARAMETER(pPager);

  if( !pCache ) return BF_ERROR;

  pEntry = sqlite3BfMapLookup(pCache, pgno);
  if( !pEntry || pEntry->locType != BF_LOC_MINI ){
    return BF_OK;  /* No mini-page to merge */
  }

  pMini = (BfMiniPage*)pEntry->pPage;
  if( !pMini ) return BF_OK;

  /* Check if mini-page has dirty records that need merging */
  if( !sqlite3BfMiniPageIsDirty(pMini) ){
    return BF_OK;  /* Nothing to merge */
  }

  /* Count dirty records before processing */
  nDirty = sqlite3BfMiniPageDirtyCount(pMini);

  /* If we have a Pager context, apply dirty records to the base page */
  if( pPager != NULL ){
    /* Create context for the callback */
    BfApplyContext applyCtx;
    applyCtx.pPager = pPager;
    applyCtx.pgno = pgno;
    
    /* Apply each dirty record to the base page */
    rc = sqlite3BfMiniPageIterate(pMini, bfApplyDirtyRecordCallback, &applyCtx);
    if( rc != BF_OK && rc != SQLITE_OK ){
      return rc;
    }
  }

  /* Consolidate the mini-page to remove unreferenced records */
  rc = sqlite3BfMiniPageConsolidate(pMini);
  if( rc != BF_OK ){
    return rc;
  }

  /* All records processed successfully - update mini-page state */
  sqlite3BfMiniPageMarkClean(pMini);

  /* Update cache statistics
  ** nMergeToBase: total number of merge operations completed
  ** Incremented by the number of dirty operations processed
  */
  pCache->nMergeToBase += nDirty;

  return BF_OK;
}

/*
** Public API: Merge a mini-page to its base page.
** Called when mini-page is full or being evicted.
**
** This keeps the merge entirely inside the BF cache layer. Dirty records are
** consolidated into the mini-page state and then marked clean so scans do not
** re-enter the pager/VDBE stack.
**
** The merge process:
** 1. Look up the mini-page in the mapping table
** 2. Check if it has dirty records (BFOP_INSERT or BFOP_DELETE)
** 3. Count dirty records
** 4. Mark the mini-page clean once consolidation is complete
** 5. Update cache statistics
**
** Return: BF_OK on success, error code otherwise
*/
int sqlite3BfCacheMerge(BfCache *pCache, u32 pgno){
  return bfCacheMergeInternal(pCache, pgno, NULL);
}

/*
** Merge with Pager context for P1 record application.
** P1 Implementation: Pass Pager to merge callback for btree record application.
**
** This is the main merge path that applies records to btree during merge.
*/
int sqlite3BfCacheMergeWithPager(BfCache *pCache, u32 pgno, Pager *pPager){
  return bfCacheMergeInternal(pCache, pgno, pPager);
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
  pEntry = sqlite3BfMapLookup(pCache, pMini->ownerPgno);
  if( pEntry == 0 || pEntry->locType != BF_LOC_MINI || pEntry->pPage != pMini ){
    return BF_OK;            /* Case 1: orphaned */
  }
  if( sqlite3BfMiniPageIsDirty(pMini) ){
    return BF_ERROR;         /* Case 3: dirty — refuse, never drop data */
  }

  /* Case 2: clean — unlink, then allow the reclaim */
  pEntry->locType = BF_LOC_NULL;
  pEntry->pPage = 0;
  return BF_OK;
}


int sqlite3BfCacheEvict(BfCache *pCache, int nTarget){
  int nEvicted = 0;

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
