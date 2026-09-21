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
** This file implements the mapping table for Bf-Tree cache.
**
** The mapping table maps page numbers to their physical locations:
**   - BF_LOC_NULL: Page not in cache
**   - BF_LOC_BASE: Only on disk at a given offset
**   - BF_LOC_MINI: Mini-page delta chain in circular buffer
**   - BF_LOC_FULL: Full page mirror in circular buffer
**
** The table uses batched allocation (1M entries per batch) for efficiency
** and includes per-entry reader-writer locks for concurrency.
*/
#include "sqliteInt.h"
#ifndef SQLITE_OMIT_BF_CACHE
#include "bf_cache.h"

#ifdef SQLITE_BF_DEBUG
#include <fcntl.h>
#include <unistd.h>
static void bfMapLogWrite(const char *op, u32 pgno, u8 locType, void *pPage){
  char buf[256];
  int off = snprintf(buf, sizeof(buf), "bfmap-%s pgno=%u loc=%d pPage=%p\n",
                     op, pgno, (int)locType, pPage);
  int fd = open("/home/yawd/Projects/tfg-stuff/sqlite/build/bfmap-writes.log",
                O_WRONLY|O_CREAT|O_APPEND, 0644);
  if( fd>=0 ){ write(fd, buf, (size_t)off); close(fd); }
}
#define BF_MAP_LOG(op,pgno,loc,pg)  bfMapLogWrite(op,pgno,loc,pg)
#else
#define BF_MAP_LOG(op,pgno,loc,pg)  ((void)0)
#endif


/*
** Initialize the mapping table in a BfCache.
*/
int sqlite3BfMapInit(BfCache *pCache){
  pCache->apMap = 0;
  pCache->nMapBatch = 0;
  return SQLITE_OK;
}

/*
** Destroy the mapping table and free all resources.
*/
void sqlite3BfMapDestroy(BfCache *pCache){
  int i;
  if( pCache->apMap ){
    for(i = 0; i < pCache->nMapBatch; i++){
      if( pCache->apMap[i] ){
        sqlite3_free(pCache->apMap[i]);
      }
    }
    sqlite3_free(pCache->apMap);
    pCache->apMap = 0;
  }
  pCache->nMapBatch = 0;
}

/*
** Ensure the mapping table has capacity for the given page number.
** Allocates new batches as needed.
*/
static int bfMapEnsureCapacity(BfCache *pCache, u32 pgno){
  int batchIdx = pgno / BF_MAP_BATCH_SIZE;
  int newBatchCount;
  BfMapEntry **apNew;
  BfMapEntry *pNewBatch;
  int i;

  /* Check if we need more batch pointers */
  if( batchIdx >= pCache->nMapBatch ){
    if( batchIdx >= BF_MAP_MAX_BATCHES ){
      return SQLITE_FULL;  /* Exceeded maximum capacity */
    }

    newBatchCount = batchIdx + 1;
    apNew = (BfMapEntry**)sqlite3_realloc64(pCache->apMap,
        sizeof(BfMapEntry*) * newBatchCount);
    if( !apNew ){
      BF_MAP_LOG("alloc-apmap-failed", pgno, 0, 0);
      return SQLITE_NOMEM;
    }

    /* Initialize new batch pointers to NULL */
    for(i = pCache->nMapBatch; i < newBatchCount; i++){
      apNew[i] = 0;
    }

    pCache->apMap = apNew;
    pCache->nMapBatch = newBatchCount;
  }

  /* Allocate the batch if needed */
  if( !pCache->apMap[batchIdx] ){
    pNewBatch = (BfMapEntry*)sqlite3_malloc64(
        sizeof(BfMapEntry) * BF_MAP_BATCH_SIZE);
    if( !pNewBatch ){
      BF_MAP_LOG("alloc-batch-failed", pgno, 0, 0);
      return SQLITE_NOMEM;
    }

    /* Initialize all entries to NULL location */
    memset(pNewBatch, 0, sizeof(BfMapEntry) * BF_MAP_BATCH_SIZE);
    pCache->apMap[batchIdx] = pNewBatch;
#ifdef SQLITE_BF_TRACE
    sqlite3_log(SQLITE_NOTICE, "bfmap: allocated batch %d (pgno %u-%u)", batchIdx, (u32)(batchIdx*BF_MAP_BATCH_SIZE), (u32)((batchIdx+1)*BF_MAP_BATCH_SIZE - 1));
#endif
  }

  return SQLITE_OK;
}

/*
** Look up a page in the mapping table.
** Returns pointer to entry, or NULL if not allocated.
*/
BfMapEntry *sqlite3BfMapLookup(BfCache *pCache, u32 pgno){
  int batchIdx = pgno / BF_MAP_BATCH_SIZE;
  int entryIdx = pgno % BF_MAP_BATCH_SIZE;

  if( batchIdx >= pCache->nMapBatch ){
    return 0;
  }
  if( !pCache->apMap[batchIdx] ){
    return 0;
  }

  return &pCache->apMap[batchIdx][entryIdx];
}

/*
** Get or create a mapping entry for a page.
** Returns pointer to entry, or NULL on allocation failure.
*/
BfMapEntry *sqlite3BfMapGetOrCreate(BfCache *pCache, u32 pgno){
  int batchIdx = pgno / BF_MAP_BATCH_SIZE;
  int entryIdx = pgno % BF_MAP_BATCH_SIZE;

  if( bfMapEnsureCapacity(pCache, pgno) != SQLITE_OK ){
    return 0;
  }

  return &pCache->apMap[batchIdx][entryIdx];
}

/*
** Insert or update a mapping entry.
*/
int sqlite3BfMapInsert(BfCache *pCache, u32 pgno, u8 locType,
    void *pPage, i64 diskOffset){
  BfMapEntry *pEntry;
  int rc;

  rc = bfMapEnsureCapacity(pCache, pgno);
  if( rc != SQLITE_OK ) return rc;

  pEntry = sqlite3BfMapGetOrCreate(pCache, pgno);
  if( !pEntry ) return SQLITE_NOMEM;

  BF_MAP_LOG("insert", pgno, locType, pPage);
  pEntry->locType = locType;
  pEntry->pPage = pPage;
  pEntry->diskOffset = diskOffset;

  return SQLITE_OK;
}

/*
** Iterate over all non-null entries in the mapping table.
** Callback returns non-zero to stop iteration.
*/
int sqlite3BfMapIterate(BfCache *pCache,
    int (*xCallback)(void*, u32, BfMapEntry*), void *pCtx){
  int batch, entry;
  u32 pgno;
  BfMapEntry *pEntry;
  int rc;

  for(batch = 0; batch < pCache->nMapBatch; batch++){
    if( !pCache->apMap[batch] ) continue;

    for(entry = 0; entry < BF_MAP_BATCH_SIZE; entry++){
      pEntry = &pCache->apMap[batch][entry];
      if( pEntry->locType == BF_LOC_NULL ) continue;

      pgno = batch * BF_MAP_BATCH_SIZE + entry;
      rc = xCallback(pCtx, pgno, pEntry);
      if( rc ) return rc;
    }
  }

  return 0;
}

/*
** Dirty-list maintenance.
**
** The mapping table is a direct-indexed array of batches, so
** sqlite3BfMapIterate costs O(highest pgno cached) whether one mini-page is
** dirty or none are.  The pre-mutation flush called it on EVERY mutation and
** the commit-time record gather on EVERY commit, which made append workloads
** quadratic (measured: 200k appends spent 17.5s here, 132x stock).  These two
** routines keep a short list of candidate pgnos instead.
**
** The list is a SUPERSET of the dirty mini-pages: entries that have since been
** flushed are dropped lazily while iterating.  It is never a subset, because
** sqlite3BfRecordWrite is the only way a mini-page becomes dirty and it always
** calls sqlite3BfDirtyListAdd.  On allocation failure the list is abandoned
** (bDirtyListOverflow) and callers fall back to the full walk, so running out
** of memory costs speed, never correctness.
*/
void sqlite3BfDirtyListAdd(BfCache *pCache, u32 pgno){
  if( pCache->bDirtyListOverflow ) return;
  if( pCache->nDirtyPg >= pCache->nDirtyPgAlloc ){
    int nNew = pCache->nDirtyPgAlloc ? pCache->nDirtyPgAlloc*2 : 64;
    u32 *aNew = (u32*)sqlite3_realloc(pCache->aDirtyPg,
                                      nNew*(int)sizeof(u32));
    if( aNew==0 ){
      pCache->bDirtyListOverflow = 1;   /* callers revert to the full walk */
      return;
    }
    pCache->aDirtyPg = aNew;
    pCache->nDirtyPgAlloc = nNew;
  }
  pCache->aDirtyPg[pCache->nDirtyPg++] = pgno;
}

/*
** The unlogged-page list.  Same shape and same contract as the dirty list
** above, answering the other question: which pages did THIS transaction dirty?
**
** The dirty list cannot answer it.  A mini-page stays dirty until its records
** reach the base page, so the dirty list holds every mini-page written since
** the last flush, and a commit that logged only one row still walked all of
** them -- measured at 24.9% of cycles on a one-row-per-transaction insert
** workload, and quadratic over a run.  Entries here are dropped as soon as the
** page has nothing unlogged left, which for a normal commit is immediately.
*/
void sqlite3BfUnlogListAdd(BfCache *pCache, u32 pgno){
  if( pCache->bUnlogOverflow ) return;
  if( pCache->nUnlogPg >= pCache->nUnlogPgAlloc ){
    int nNew = pCache->nUnlogPgAlloc ? pCache->nUnlogPgAlloc*2 : 64;
    u32 *aNew = (u32*)sqlite3_realloc(pCache->aUnlogPg,
                                      nNew*(int)sizeof(u32));
    if( aNew==0 ){
      pCache->bUnlogOverflow = 1;   /* commit reverts to the dirty list */
      return;
    }
    pCache->aUnlogPg = aNew;
    pCache->nUnlogPgAlloc = nNew;
  }
  pCache->aUnlogPg[pCache->nUnlogPg++] = pgno;
}

int sqlite3BfUnlogListIterate(BfCache *pCache,
    int (*xCallback)(void*, u32, BfMapEntry*), void *pCtx){
  int i = 0;
#ifdef SQLITE_BF_NO_UNLOGLIST
  return 1;                      /* ablation: force the dirty-list walk */
#endif
  if( pCache->bUnlogOverflow ) return 1;   /* caller falls back */
  while( i < pCache->nUnlogPg ){
    u32 pgno = pCache->aUnlogPg[i];
    BfMapEntry *pEntry = sqlite3BfMapLookup(pCache, pgno);
    BfMiniPage *pMini = 0;
    if( pEntry && pEntry->locType==BF_LOC_MINI ){
      pMini = (BfMiniPage*)pEntry->pPage;
    }
    if( pMini==0 || (pMini->flags & BF_MINI_F_UNLOGGED)==0 ){
      /* Gone, or everything on it is logged.  Drop it; order is irrelevant. */
      pCache->aUnlogPg[i] = pCache->aUnlogPg[--pCache->nUnlogPg];
      continue;
    }
    if( xCallback(pCtx, pgno, pEntry) ) return 0;
    /* Re-test rather than assume the callback got through the whole page: a
    ** gather that stopped early (out of space, I/O error) must keep the entry,
    ** or its records would never be logged. */
    if( (pMini->flags & BF_MINI_F_UNLOGGED)==0 ){
      pCache->aUnlogPg[i] = pCache->aUnlogPg[--pCache->nUnlogPg];
      continue;
    }
    i++;
  }
  return 0;
}

void sqlite3BfUnlogListReset(BfCache *pCache){
  pCache->nUnlogPg = 0;
  pCache->bUnlogOverflow = 0;
}

int sqlite3BfDirtyListIterate(BfCache *pCache,
    int (*xCallback)(void*, u32, BfMapEntry*), void *pCtx){
  int i = 0;
#ifdef SQLITE_BF_NO_DIRTYLIST
  return 1;                      /* ablation: force the full-map walk */
#endif
  if( pCache->bDirtyListOverflow ) return 1;   /* caller does the full walk */
  while( i < pCache->nDirtyPg ){
    u32 pgno = pCache->aDirtyPg[i];
    BfMapEntry *pEntry = sqlite3BfMapLookup(pCache, pgno);
    BfMiniPage *pMini = 0;
    if( pEntry && pEntry->locType==BF_LOC_MINI ){
      pMini = (BfMiniPage*)pEntry->pPage;
    }
    if( pMini==0 || (pMini->flags & BF_MINI_F_DIRTY)==0 ){
      /* Stale: gone, or already flushed.  Drop it (order is irrelevant). */
      pCache->aDirtyPg[i] = pCache->aDirtyPg[--pCache->nDirtyPg];
      continue;
    }
    if( xCallback(pCtx, pgno, pEntry) ) return 0;
    /* The callback may have cleaned this entry; re-test it next round rather
    ** than assuming, so a refused flush (e.g. no write transaction) keeps it. */
    if( (pMini->flags & BF_MINI_F_DIRTY)==0 ){
      pCache->aDirtyPg[i] = pCache->aDirtyPg[--pCache->nDirtyPg];
      continue;
    }
    i++;
  }
  return 0;
}

/*
** Space accounting for the mapping table and the mini-pages it points at.
**
** One walk, all five gauges, because they are only read by PRAGMA
** bf_cache_stats at the end of a run and every one of them is needed to answer
** the same question: what does the record cache actually COST per cached row?
**
** The mapping table is a sparse direct index -- a 256-entry batch of 24-byte
** entries (6 KiB) is allocated the first time ANY page in that 256-page range
** is touched.  Under a skewed-but-scattered read workload the hot pages spread
** thinly over the whole file, so nearly every batch gets allocated to hold a
** handful of live entries.  nBatches vs nEntries is what makes that visible:
** nEntries/(nBatches*256) is the map's occupancy, and a low value means the
** map is spending 6 KiB to remember one page.
**
** Any of the out pointers may be NULL.
*/
void sqlite3BfMapSpaceStats(
  BfCache *pCache,
  u64 *pnBatches,      /* Allocated 256-entry batches */
  u64 *pnEntries,      /* Live (non-NULL) map entries */
  u64 *pnMiniPages,    /* Live BF_LOC_MINI entries */
  u64 *pnRecords,      /* Records held across all mini-pages */
  u64 *pnMiniBytes,    /* Bytes of mini-page allocated (nodeSize sum) */
  u64 *pnCapacity      /* Circular buffer capacity ACTUALLY in force */
){
  int batch, entry;
  u64 nBatches = 0, nEntries = 0, nMini = 0, nRec = 0, nBytes = 0;

  if( pCache ){
    for(batch = 0; batch < pCache->nMapBatch; batch++){
      if( !pCache->apMap[batch] ) continue;
      nBatches++;
      for(entry = 0; entry < BF_MAP_BATCH_SIZE; entry++){
        BfMapEntry *pE = &pCache->apMap[batch][entry];
        if( pE->locType == BF_LOC_NULL ) continue;
        nEntries++;
        if( pE->locType == BF_LOC_MINI && pE->pPage ){
          BfMiniPage *pMini = (BfMiniPage*)pE->pPage;
          nMini++;
          nRec += pMini->metaCount;
          nBytes += pMini->nodeSize;
        }
      }
    }
  }

  if( pnCapacity )  *pnCapacity  = pCache ? pCache->cb.capacity : 0;
  if( pnBatches )   *pnBatches   = nBatches;
  if( pnEntries )   *pnEntries   = nEntries;
  if( pnMiniPages ) *pnMiniPages = nMini;
  if( pnRecords )   *pnRecords   = nRec;
  if( pnMiniBytes ) *pnMiniBytes = nBytes;
}

#endif /* !defined(SQLITE_OMIT_BF_CACHE) */
