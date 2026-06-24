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
** Remove a mapping entry (set to NULL location).
*/
int sqlite3BfMapRemove(BfCache *pCache, u32 pgno){
  BfMapEntry *pEntry = sqlite3BfMapLookup(pCache, pgno);

  if( pEntry ){
    BF_MAP_LOG("remove", pgno, pEntry->locType, pEntry->pPage);
    pEntry->locType = BF_LOC_NULL;
    pEntry->pPage = 0;
    pEntry->diskOffset = 0;
  }

  return SQLITE_OK;
}

/*
** Update just the location type and page pointer.
*/
int sqlite3BfMapUpdateLocation(BfCache *pCache, u32 pgno, u8 locType, void *pPage){
  BfMapEntry *pEntry = sqlite3BfMapLookup(pCache, pgno);

  if( !pEntry ){
    return sqlite3BfMapInsert(pCache, pgno, locType, pPage, -1);
  }

  BF_MAP_LOG("update", pgno, locType, pPage);
  pEntry->locType = locType;
  pEntry->pPage = pPage;

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
** Count non-null entries in the mapping table.
*/
int sqlite3BfMapCount(BfCache *pCache){
  int batch, entry;
  int count = 0;

  for(batch = 0; batch < pCache->nMapBatch; batch++){
    if( !pCache->apMap[batch] ) continue;

    for(entry = 0; entry < BF_MAP_BATCH_SIZE; entry++){
      if( pCache->apMap[batch][entry].locType != BF_LOC_NULL ){
        count++;
      }
    }
  }

  return count;
}

/*
** Count mini-pages in the mapping table.
*/
int sqlite3BfMapMiniPageCount(BfCache *pCache){
  int batch, entry;
  int count = 0;

  for(batch = 0; batch < pCache->nMapBatch; batch++){
    if( !pCache->apMap[batch] ) continue;

    for(entry = 0; entry < BF_MAP_BATCH_SIZE; entry++){
      if( pCache->apMap[batch][entry].locType == BF_LOC_MINI ){
        count++;
      }
    }
  }

  return count;
}

/*
** Count full pages in the mapping table.
*/
int sqlite3BfMapFullPageCount(BfCache *pCache){
  int batch, entry;
  int count = 0;

  for(batch = 0; batch < pCache->nMapBatch; batch++){
    if( !pCache->apMap[batch] ) continue;

    for(entry = 0; entry < BF_MAP_BATCH_SIZE; entry++){
      if( pCache->apMap[batch][entry].locType == BF_LOC_FULL ){
        count++;
      }
    }
  }

  return count;
}

#endif /* !defined(SQLITE_OMIT_BF_CACHE) */
