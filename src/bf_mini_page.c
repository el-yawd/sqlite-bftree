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
** This file implements mini-page operations for the Bf-Tree cache.
**
** Mini-pages are variable-length (64-4096 bytes) cached subsets of disk
** pages. Records are stored sorted for efficient binary search, with
** operation types (INSERT, DELETE, CACHE, PHANTOM) for tracking dirty state.
**
** Layout:
**   [BfMiniPage header (24 bytes)]
**   [BfKVMeta array - grows forward]
**   [free space]
**   [key/value data - grows backward from end]
*/
#include "sqliteInt.h"
#ifndef SQLITE_OMIT_BF_CACHE
#include "bf_cache.h"

#ifdef SQLITE_BF_DEBUG
#include <unistd.h>
#include <fcntl.h>
static void bfMiniTrace(const char *zTag, const void *pPtr, int nSize){
  char zBuf[256];
  int n = snprintf(zBuf, sizeof(zBuf), "bfmini: %s ptr=%p size=%d\n", zTag, pPtr, nSize);
  int fd = open("/home/yawd/Projects/tfg-stuff/sqlite/build/bf-allocs.log",
                O_WRONLY|O_CREAT|O_APPEND, 0644);
  if( fd>=0 ){ write(fd, zBuf, n); close(fd); }
}
#define BF_MINI_TRACE(t,p,n)  bfMiniTrace(t,p,n)
#else
#define BF_MINI_TRACE(t,p,n)  ((void)0)
#endif

/*
** Minimum overhead per record: BfKVMeta (8 bytes) + at least 1 byte key.
*/
#define BF_MIN_RECORD_OVERHEAD  (sizeof(BfKVMeta) + 1)

/*
** Get pointer to the start of key/value data area (grows backward from end).
*/
static u8 *bfMiniPageDataEnd(BfMiniPage *pMini){
  return (u8*)pMini + pMini->nodeSize;
}

/*
** Get pointer to the BfKVMeta array.
*/
static BfKVMeta *bfMiniPageMeta(BfMiniPage *pMini){
  return (BfKVMeta*)((u8*)pMini + sizeof(BfMiniPage));
}

/*
** Get pointer to key data for a record.
*/
static u8 *bfGetKeyPtr(BfMiniPage *pMini, BfKVMeta *pMeta){
  return bfMiniPageDataEnd(pMini) - pMeta->offset;
}

/*
** Get pointer to value data for a record.
*/
static u8 *bfGetValuePtr(BfMiniPage *pMini, BfKVMeta *pMeta){
  u16 keyLen = BF_KV_KEY_LEN(pMeta);
  return bfMiniPageDataEnd(pMini) - pMeta->offset + keyLen;
}

/*
** Compare two keys.
** Returns <0 if pKey1 < pKey2, 0 if equal, >0 if pKey1 > pKey2.
*/
static int bfKeyCompare(const u8 *pKey1, int nKey1, const u8 *pKey2, int nKey2){
  int n = nKey1 < nKey2 ? nKey1 : nKey2;
  int rc = memcmp(pKey1, pKey2, n);
  if( rc == 0 ){
    rc = nKey1 - nKey2;
  }
  return rc;
}

/*
** Compare search key with record key, using preview bytes first.
** Returns <0, 0, >0 similar to memcmp.
*/
static int bfKeyCompareWithPreview(BfMiniPage *pMini, BfKVMeta *pMeta,
    const u8 *pSearchKey, int nSearchKey){
  u8 *pRecordKey;
  int nRecordKey;
  int cmp;

  /* Compare preview bytes first (optimization) */
  if( nSearchKey >= 2 ){
    cmp = (int)pSearchKey[0] - (int)pMeta->preview[0];
    if( cmp != 0 ) return cmp;
    cmp = (int)pSearchKey[1] - (int)pMeta->preview[1];
    if( cmp != 0 ) return cmp;
  }

  /* Full key comparison */
  pRecordKey = bfGetKeyPtr(pMini, pMeta);
  nRecordKey = BF_KV_KEY_LEN(pMeta);
  return bfKeyCompare(pSearchKey, nSearchKey, pRecordKey, nRecordKey);
}

/*
** Binary search for a key in the mini-page.
** Returns index of matching record, or insertion point if not found.
** Sets *pFound to 1 if exact match, 0 otherwise.
*/
static int bfBinarySearch(BfMiniPage *pMini, const u8 *pKey, int nKey, int *pFound){
  BfKVMeta *aMeta = bfMiniPageMeta(pMini);
  int lo = 0;
  int hi = pMini->metaCount - 1;
  int mid, cmp;

  *pFound = 0;

  while( lo <= hi ){
    mid = (lo + hi) / 2;
    /* cmp = sign(searchKey - record[mid]) */
    cmp = bfKeyCompareWithPreview(pMini, &aMeta[mid], pKey, nKey);

    if( cmp < 0 ){
      hi = mid - 1;   /* searchKey < record[mid]: look in the lower half */
    }else if( cmp > 0 ){
      lo = mid + 1;   /* searchKey > record[mid]: look in the upper half */
    }else{
      *pFound = 1;
      return mid;
    }
  }

  return lo;  /* Insertion point */
}

/*
** Initialize a mini-page.
*/
void sqlite3BfMiniPageInit(BfMiniPage *pMini, u16 size, i64 baseDiskOffset){
  memset(pMini, 0, sizeof(BfMiniPage));
  pMini->nodeSize = size;
  pMini->metaCount = 0;
  pMini->freeSpace = size - sizeof(BfMiniPage);
  pMini->flags = 0;
  pMini->baseDiskOffset = baseDiskOffset;
  /* Ownership back-pointers are assigned by the mapping layer
  ** (sqlite3BfRecordWrite) once the page number is known; zero means
  ** "not yet linked into the map". */
  pMini->ownerPgno = 0;
  pMini->rootPgno = 0;
}

/*
** Calculate space needed for a new record.
*/
static u32 bfRecordSpace(int nKey, int nVal){
  return sizeof(BfKVMeta) + nKey + nVal;
}

/*
** Check how much space remains in the mini-page.
*/
int sqlite3BfMiniPageSpaceRemaining(BfMiniPage *pMini){
  return pMini->freeSpace;
}

/*
** Check if mini-page needs to be merged (too full or too many records).
*/
int sqlite3BfMiniPageNeedsMerge(BfMiniPage *pMini){
  /* Merge when 90% full or has many records */
  u32 used = pMini->nodeSize - pMini->freeSpace - sizeof(BfMiniPage);
  return (used > (pMini->nodeSize * 9 / 10)) ||
         (pMini->metaCount > 100);
}

/*
** Get the next size class for upgrading a mini-page.
** Returns 0 if already at maximum size.
*/
u32 sqlite3BfMiniPageNextSizeClass(BfMiniPage *pMini, u32 *aSizeClass){
  int i;
  /* Ascending: the SMALLEST class larger than the current node.
  **
  ** This loop used to run downward from BF_SIZE_CLASS_COUNT-1 and return the
  ** first class greater than nodeSize -- which, since the largest class is
  ** always greater, meant it returned BF_MAX_MINI_PAGE (4096) on the very
  ** first iteration, every time.  Every mini-page that overflowed its 64-byte
  ** initial allocation therefore jumped straight to 4096 bytes, so a ~110-byte
  ** row occupied a 4 KiB mini-page: 37x space amplification, a ring that held
  ** ~63k records instead of ~2M, and constant eviction churn.  The header
  ** comment on BF_MIN_MINI_PAGE ("double until BF_MAX_MINI_PAGE") describes
  ** the intent this now implements. */
  for(i = 0; i < BF_SIZE_CLASS_COUNT; i++){
    if( aSizeClass[i] > pMini->nodeSize ){
      return aSizeClass[i];
    }
  }
  return 0;  /* Already at maximum */
}

/*
** The smallest size class that can hold this mini-page's LIVE records plus a
** new record of nKey+nVal bytes.  Returns 0 if no class is large enough (or
** the page is already at the largest that would help).
**
** Why this exists rather than repeated NextSizeClass steps: the caller
** upgrades at most once per write, so a single "next class up" is not enough
** when the record needs several steps.  From the 64-byte initial allocation
** one step reaches 128, which holds a payload of at most
** 128 - sizeof(BfMiniPage) - sizeof(BfKVMeta) = 96 bytes -- exactly the
** measured cliff where write-back buffering started refusing records and
** falling back to the base-page path.  Choosing the fitting class directly
** makes one upgrade always sufficient.
**
** Live content is summed from the meta array rather than taken as
** nodeSize-freeSpace: freeSpace excludes bytes stranded by fragmentation,
** which would overstate the requirement.  The upgrade path copies through
** sqlite3BfMiniPageCopy, which repacks, so the post-copy occupancy is exactly
** this sum.
*/
u32 sqlite3BfMiniPageSizeClassFor(
  BfMiniPage *pMini,
  int nKey,
  int nVal,
  u32 *aSizeClass
){
  BfKVMeta *aMeta = bfMiniPageMeta(pMini);
  u32 used = (u32)sizeof(BfMiniPage);
  u32 needed;
  int i;

  for(i = 0; i < pMini->metaCount; i++){
    used += bfRecordSpace(BF_KV_KEY_LEN(&aMeta[i]), BF_KV_VALUE_LEN(&aMeta[i]));
  }
  needed = used + bfRecordSpace(nKey, nVal);

  for(i = 0; i < BF_SIZE_CLASS_COUNT; i++){
    if( aSizeClass[i] > pMini->nodeSize && aSizeClass[i] >= needed ){
      return aSizeClass[i];
    }
  }
  return 0;
}

/*
** Insert a record into the mini-page.
** Records are kept sorted by key for efficient binary search.
**
** Returns BF_OK on success, BF_MINI_PAGE_FULL if no space.
*/
int sqlite3BfMiniPageInsert(BfMiniPage *pMini,
    const void *pKey, int nKey, const void *pVal, int nVal, u8 opType){
  BfKVMeta *aMeta;
  BfKVMeta *pNewMeta;
  u8 *pDataEnd;
  u8 *pDst;
  u32 needed;
  int idx, found, i;

  needed = bfRecordSpace(nKey, nVal);
  aMeta = bfMiniPageMeta(pMini);
  pDataEnd = bfMiniPageDataEnd(pMini);

  /* Binary search for insertion point.  This is done BEFORE the free-space
  ** check: an update of an existing key whose new value is no larger than the
  ** old one (in particular a DELETE tombstone, nVal==0) is an in-place
  ** overwrite that consumes no additional space, and must succeed even when
  ** the mini-page is otherwise full.  Gating it on free space would silently
  ** drop tombstones on a maxed-out mini-page, leaving deleted rows visible. */
  idx = bfBinarySearch(pMini, (const u8*)pKey, nKey, &found);

  if( found ){
    /* Key exists — update the existing record. */
    pNewMeta = &aMeta[idx];
    int oldValLen = BF_KV_VALUE_LEN(pNewMeta);

    if( nVal <= oldValLen ){
      /* New value fits in the space already reserved for the old value.
      ** Overwrite in place: no change to freeSpace or offset arrays. */
      pDst = bfGetValuePtr(pMini, pNewMeta);
      assert( pDst >= (u8*)pMini );
      assert( (pDst + nVal) <= ((u8*)pMini + pMini->nodeSize) );
      if( nVal > 0 ) memcpy(pDst, pVal, nVal);
      BF_KV_SET_VALUE_LEN(pNewMeta, nVal);
      BF_KV_SET_OP_TYPE(pNewMeta, opType);
      BF_KV_SET_REF(pNewMeta, 1);
      if( opType==BFOP_INSERT || opType==BFOP_DELETE ){
        pMini->flags |= BF_MINI_F_DIRTY;
      }
      BF_KV_SET_LOGGED(pNewMeta, 0);  /* fresh mutation: not yet in the WAL */
      return BF_OK;
    }

    /* New value is larger than old; in-place overwrite is impossible.
    ** Removing the old meta slot frees sizeof(BfKVMeta); after that we need
    ** nKey+nVal of fresh data space.  Verify it fits before mutating. */
    if( (u32)(nKey + nVal) > pMini->freeSpace ){
      /* The larger value does not fit in place.  For a CLEAN read-cache
      ** overwrite the stale existing record MUST NOT be left behind: a reader
      ** would serve the outdated (smaller) bytes.  Drop its meta slot so the
      ** key reads as absent — the caller then re-inserts the fresh value into
      ** an upgraded mini-page (sqlite3BfRecordWrite) or, if no upgrade is
      ** possible, a later read re-promotes from the authoritative base page.
      ** Dirty records (BFOP_INSERT/BFOP_DELETE) are never dropped here: that
      ** would lose buffered data the base tree does not yet hold. */
      if( opType==BFOP_CACHE || opType==BFOP_PHANTOM ){
        for(i = idx; i < pMini->metaCount - 1; i++){
          aMeta[i] = aMeta[i+1];
        }
        pMini->metaCount--;
        pMini->freeSpace += (u16)sizeof(BfKVMeta);
      }
      return BF_MINI_PAGE_FULL;
    }

    /* Remove the existing meta entry (shift array left) and re-insert at the
    ** correct sorted position with the new data.  The old key+value bytes in
    ** the data area become fragmentation reclaimed on the next
    ** sqlite3BfMiniPageConsolidate() call. */
    for(i = idx; i < pMini->metaCount - 1; i++){
      aMeta[i] = aMeta[i+1];
    }
    pMini->metaCount--;
    pMini->freeSpace += (u16)sizeof(BfKVMeta);

    /* Re-locate the insertion position in the now-shorter array.
    ** The key is absent, so found must be 0 after this call. */
    idx = bfBinarySearch(pMini, (const u8*)pKey, nKey, &found);
    assert( !found );
  }else{
    /* Brand-new record: needs meta slot + key + value. */
    if( needed > pMini->freeSpace ){
      return BF_MINI_PAGE_FULL;
    }
  }

  /* Calculate new data offset.
  **
  ** The data area grows backward from pDataEnd: a record's bytes live at
  ** [pDataEnd - offset, pDataEnd - offset + nKey + nVal).  The largest
  ** existing offset therefore marks the current frontier (lowest address
  ** used); the next record must be placed beyond it.
  **
  ** This MUST be computed over the current (pre-shift) metadata: the shift
  ** below duplicates a slot and would otherwise hide the true frontier from
  ** the scan, producing a colliding offset. */
  u16 newOffset;
  if( pMini->metaCount == 0 ){
    newOffset = nKey + nVal;
  }else{
    u16 maxOffset = 0;
    for(i = 0; i < pMini->metaCount; i++){
      if( aMeta[i].offset > maxOffset ){
        maxOffset = aMeta[i].offset;
      }
    }
    newOffset = maxOffset + nKey + nVal;
  }

  /* Shift existing metadata to make room */
  for(i = pMini->metaCount; i > idx; i--){
    aMeta[i] = aMeta[i-1];
  }

  /* Write key/value data (from end of page, growing backward) */
  pDst = pDataEnd - newOffset;
  assert( pDst >= (u8*)pMini );
  assert( (pDst + nKey + nVal) <= ((u8*)pMini + pMini->nodeSize) );
  memcpy(pDst, pKey, nKey);
  memcpy(pDst + nKey, pVal, nVal);

  /* Initialize new metadata */
  pNewMeta = &aMeta[idx];
  pNewMeta->offset = newOffset;
  BF_KV_SET_KEY_LEN(pNewMeta, nKey);
  BF_KV_SET_OP_TYPE(pNewMeta, opType);
  BF_KV_SET_VALUE_LEN(pNewMeta, nVal);
  BF_KV_SET_REF(pNewMeta, 1);
  BF_KV_SET_LOGGED(pNewMeta, 0);  /* brand-new record: not yet in the WAL */
  if( opType==BFOP_INSERT || opType==BFOP_DELETE ){
    pMini->flags |= BF_MINI_F_DIRTY;
  }

  /* Set preview bytes */
  pNewMeta->preview[0] = nKey >= 1 ? ((const u8*)pKey)[0] : 0;
  pNewMeta->preview[1] = nKey >= 2 ? ((const u8*)pKey)[1] : 0;

  /* Update page header */
  pMini->metaCount++;
  pMini->freeSpace -= needed;

  return BF_OK;
}

/*
** Look up an exact key in the mini-page and report its operation type.
**
** Unlike sqlite3BfMiniPageSearch (which collapses BFOP_DELETE and BFOP_PHANTOM
** into BF_DELETED), this returns the raw BFOP_* type so the forward merge scan
** can tell a dirty write-back tombstone (BFOP_DELETE — suppresses a base cell)
** from a clean phantom (BFOP_PHANTOM — does not).  Returns 1 and sets *pOp when
** the key is present (any op); returns 0 if the key is absent.
*/
int sqlite3BfMiniPageLookupOp(BfMiniPage *pMini,
    const void *pKey, int nKey, u8 *pOp){
  int idx, found;
  BfKVMeta *aMeta;
  if( pMini->metaCount==0 ) return 0;
  aMeta = bfMiniPageMeta(pMini);
  idx = bfBinarySearch(pMini, (const u8*)pKey, nKey, &found);
  if( !found ) return 0;
  if( pOp ) *pOp = BF_KV_OP_TYPE(&aMeta[idx]);
  return 1;
}

/*
** Search for a record in the mini-page.
**
** Returns BF_OK if found and copies value to pBuf (up to *pnBuf bytes).
** Returns BF_NOT_FOUND if not found.
** On success, *pnBuf is set to actual value length.
*/
int sqlite3BfMiniPageSearch(BfMiniPage *pMini,
    const void *pKey, int nKey, void *pBuf, int *pnBuf){
  int idx, found;
  BfKVMeta *aMeta;
  BfKVMeta *pMeta;
  u8 opType;
  u8 *pVal;
  int nVal;
  int nCopy;

  if( pMini->metaCount == 0 ){
    return BF_NOT_FOUND;
  }

  aMeta = bfMiniPageMeta(pMini);
  idx = bfBinarySearch(pMini, (const u8*)pKey, nKey, &found);

  if( !found ){
    return BF_NOT_FOUND;
  }

  pMeta = &aMeta[idx];
  opType = BF_KV_OP_TYPE(pMeta);

  /* Check if this is a deletion marker */
  if( opType == BFOP_DELETE || opType == BFOP_PHANTOM ){
    return BF_DELETED;  /* Record was deleted */
  }

  /* Mark as referenced */
  BF_KV_SET_REF(pMeta, 1);

  /* Copy value */
  pVal = bfGetValuePtr(pMini, pMeta);
  nVal = BF_KV_VALUE_LEN(pMeta);
  nCopy = nVal;
  if( pnBuf && *pnBuf < nCopy ){
    nCopy = *pnBuf;
  }

  if( pBuf && nCopy > 0 ){
    assert( pVal >= (u8*)pMini );
    assert( (pVal + nCopy) <= ((u8*)pMini + pMini->nodeSize) );
    memcpy(pBuf, pVal, nCopy);
  }

  if( pnBuf ){
    *pnBuf = nVal;
  }

  return BF_OK;
}

/*
** Delete a record from the mini-page (insert tombstone).
**
** Returns BF_OK on success, BF_MINI_PAGE_FULL if no space.
*/
int sqlite3BfMiniPageDelete(BfMiniPage *pMini, const void *pKey, int nKey){
  /* Insert a tombstone record with empty value */
  return sqlite3BfMiniPageInsert(pMini, pKey, nKey, "", 0, BFOP_DELETE);
}

/*
** Count the number of dirty records in the mini-page.
** Dirty records are INSERT or DELETE operations that need to be written.
*/
int sqlite3BfMiniPageDirtyCount(BfMiniPage *pMini){
  BfKVMeta *aMeta = bfMiniPageMeta(pMini);
  int i, count = 0;
  u8 opType;

  for(i = 0; i < pMini->metaCount; i++){
    opType = BF_KV_OP_TYPE(&aMeta[i]);
    if( opType == BFOP_INSERT || opType == BFOP_DELETE ){
      count++;
    }
  }
  return count;
}

/*
** Check if mini-page has any dirty records.
*/
int sqlite3BfMiniPageIsDirty(BfMiniPage *pMini){
  /* O(1) for a clean mini-page: the flag is only ever set by a dirty record
  ** write and cleared by MarkClean, so "clear" is authoritative. */
  if( (pMini->flags & BF_MINI_F_DIRTY)==0 ) return 0;
  /* Stop at the FIRST dirty record.  Counting them all made this the hottest
  ** function in the profile (92% of samples on an append workload): the
  ** pre-mutation flush calls it for every mini-page in the map, on every
  ** mutation, and each call was O(records-in-mini-page). */
  BfKVMeta *aMeta = bfMiniPageMeta(pMini);
  int i;
  for(i = 0; i < pMini->metaCount; i++){
    u8 opType = BF_KV_OP_TYPE(&aMeta[i]);
    if( opType == BFOP_INSERT || opType == BFOP_DELETE ) return 1;
  }
  return 0;
}

/*
** Convert all dirty INSERT records to CACHE (clean).
** Called after mini-page is merged to base page.
*/
void sqlite3BfMiniPageMarkClean(BfMiniPage *pMini){
  BfKVMeta *aMeta = bfMiniPageMeta(pMini);
  int i;
  u8 opType;

  pMini->flags &= ~BF_MINI_F_DIRTY;   /* every dirty record becomes clean below */

  for(i = 0; i < pMini->metaCount; i++){
    opType = BF_KV_OP_TYPE(&aMeta[i]);
    if( opType == BFOP_INSERT ){
      BF_KV_SET_OP_TYPE(&aMeta[i], BFOP_CACHE);
      BF_KV_SET_LOGGED(&aMeta[i], 0);   /* now in base: no pending WAL log */
    }else if( opType == BFOP_DELETE ){
      BF_KV_SET_OP_TYPE(&aMeta[i], BFOP_PHANTOM);
      BF_KV_SET_LOGGED(&aMeta[i], 0);
    }
  }
}

/*
** Consolidate the mini-page by removing unreferenced records.
** This compacts the page and reclaims space.
*/
int sqlite3BfMiniPageConsolidate(BfMiniPage *pMini){
  BfKVMeta *aMeta = bfMiniPageMeta(pMini);
  u8 *pDataEnd = bfMiniPageDataEnd(pMini);
  int i, j;
  int keepCount = 0;
  u16 newOffset = 0;
  u8 *pSrc, *pDst;
  int nKey, nVal;
  int rc;

  /* First pass: count records to keep */
  for(i = 0; i < pMini->metaCount; i++){
    if( BF_KV_IS_REF(&aMeta[i]) ){
      keepCount++;
    }
  }

  if( keepCount == pMini->metaCount ){
    /* Nothing to consolidate */
    return BF_OK;
  }

  /* Create temporary buffer for compaction */
  u32 tempSize = pMini->nodeSize;
  u8 *pTemp = sqlite3_malloc(tempSize);
  if( !pTemp ) return SQLITE_NOMEM;
  BF_MINI_TRACE("temp-alloc", pTemp, (int)tempSize);

  BfMiniPage *pNewMini = (BfMiniPage*)pTemp;
  sqlite3BfMiniPageInit(pNewMini, pMini->nodeSize, pMini->baseDiskOffset);
  pNewMini->ownerPgno = pMini->ownerPgno;
  pNewMini->rootPgno = pMini->rootPgno;

  /* Copy referenced records */
  for(i = 0; i < pMini->metaCount; i++){
    if( BF_KV_IS_REF(&aMeta[i]) ){
      pSrc = bfGetKeyPtr(pMini, &aMeta[i]);
      nKey = BF_KV_KEY_LEN(&aMeta[i]);
      nVal = BF_KV_VALUE_LEN(&aMeta[i]);

      rc = sqlite3BfMiniPageInsert(pNewMini, pSrc, nKey, pSrc + nKey, nVal,
                                   BF_KV_OP_TYPE(&aMeta[i]));
      if( rc!=BF_OK ){
        BF_MINI_TRACE("temp-free", pTemp, (int)tempSize);
        sqlite3_free(pTemp);
        return rc;
      }
    }
  }

  /* Copy back */
  memcpy(pMini, pNewMini, pMini->nodeSize);
  BF_MINI_TRACE("temp-free", pTemp, (int)tempSize);
  sqlite3_free(pTemp);

  return BF_OK;
}

/*
** Clear reference bits on all records.
** Called at start of eviction cycle.
*/
void sqlite3BfMiniPageClearRefs(BfMiniPage *pMini){
  BfKVMeta *aMeta = bfMiniPageMeta(pMini);
  int i;

  for(i = 0; i < pMini->metaCount; i++){
    BF_KV_SET_REF(&aMeta[i], 0);
  }
}

/*
** Iterate over all records in the mini-page.
** Callback receives key, value, and operation type.
** Return non-zero from callback to stop iteration.
*/
int sqlite3BfMiniPageIterate(BfMiniPage *pMini,
    int (*xCallback)(void*, const u8*, int, const u8*, int, u8),
    void *pCtx){
  BfKVMeta *aMeta = bfMiniPageMeta(pMini);
  int i, rc;
  u8 *pKey, *pVal;
  int nKey, nVal;
  u8 opType;

  for(i = 0; i < pMini->metaCount; i++){
    pKey = bfGetKeyPtr(pMini, &aMeta[i]);
    nKey = BF_KV_KEY_LEN(&aMeta[i]);
    pVal = bfGetValuePtr(pMini, &aMeta[i]);
    nVal = BF_KV_VALUE_LEN(&aMeta[i]);
    opType = BF_KV_OP_TYPE(&aMeta[i]);

    rc = xCallback(pCtx, pKey, nKey, pVal, nVal, opType);
    if( rc ) return rc;
  }

  return 0;
}

/*
** Number of records (any op type) in the sorted meta array.
*/
int sqlite3BfMiniPageCount(BfMiniPage *pMini){
  return pMini->metaCount;
}

/*
** Random access to the record at sorted index ix.  Returns 1 and fills the
** requested out-params, or 0 if ix is out of range.  Pointers reference the
** mini-page's own storage; valid only until the next mutation of pMini.
*/
int sqlite3BfMiniPageAt(BfMiniPage *pMini, int ix,
    const u8 **ppKey, int *pnKey, const u8 **ppVal, int *pnVal, u8 *pOp){
  BfKVMeta *aMeta;
  if( ix<0 || ix>=pMini->metaCount ) return 0;
  aMeta = bfMiniPageMeta(pMini);
  if( ppKey ) *ppKey = bfGetKeyPtr(pMini, &aMeta[ix]);
  if( pnKey ) *pnKey = BF_KV_KEY_LEN(&aMeta[ix]);
  if( ppVal ) *ppVal = bfGetValuePtr(pMini, &aMeta[ix]);
  if( pnVal ) *pnVal = BF_KV_VALUE_LEN(&aMeta[ix]);
  if( pOp )   *pOp   = BF_KV_OP_TYPE(&aMeta[ix]);
  return 1;
}

/*
** Phase 2 (WAL) commit-gather support.
**
** sqlite3BfMiniPageDirtyUnloggedAt: like sqlite3BfMiniPageAt, but only reports
** the record at index ix when it is DIRTY (BFOP_INSERT/BFOP_DELETE) AND its
** WAL-logged flag is clear — i.e. an op that still needs to be written to the
** WAL as a physiological record frame this generation.  Returns 1 and fills the
** out-params in that case, 0 otherwise (clean, already-logged, or out of range).
**
** sqlite3BfMiniPageMarkLoggedAt: set the logged flag on record ix, called once
** the record's op has been safely staged into a WAL record-batch.  Setting only
** a bit in an existing meta slot performs no array shift, so it is safe to call
** while walking the mini-page by index.
*/
int sqlite3BfMiniPageDirtyUnloggedAt(BfMiniPage *pMini, int ix,
    const u8 **ppKey, int *pnKey, const u8 **ppVal, int *pnVal, u8 *pOp){
  BfKVMeta *aMeta;
  u8 op;
  if( ix<0 || ix>=pMini->metaCount ) return 0;
  aMeta = bfMiniPageMeta(pMini);
  op = BF_KV_OP_TYPE(&aMeta[ix]);
  if( op!=BFOP_INSERT && op!=BFOP_DELETE ) return 0;
  if( BF_KV_IS_LOGGED(&aMeta[ix]) ) return 0;
  if( ppKey ) *ppKey = bfGetKeyPtr(pMini, &aMeta[ix]);
  if( pnKey ) *pnKey = BF_KV_KEY_LEN(&aMeta[ix]);
  if( ppVal ) *ppVal = bfGetValuePtr(pMini, &aMeta[ix]);
  if( pnVal ) *pnVal = BF_KV_VALUE_LEN(&aMeta[ix]);
  if( pOp )   *pOp   = op;
  return 1;
}

void sqlite3BfMiniPageMarkLoggedAt(BfMiniPage *pMini, int ix){
  BfKVMeta *aMeta;
  if( ix<0 || ix>=pMini->metaCount ) return;
  aMeta = bfMiniPageMeta(pMini);
  BF_KV_SET_LOGGED(&aMeta[ix], 1);
}

/*
** Drop every CLEAN record (BFOP_CACHE / BFOP_PHANTOM), keeping the dirty ones.
**
** Used when a leaf's KEY RANGE changes under a balance: records are keyed by
** leaf, so a clean record left behind on a leaf that no longer owns that key is
** not merely useless, it is WRONG -- a later rebalance can hand the range back
** and the stale entry then answers point probes ("row exists" for a row that
** was deleted).  Dirty records are position-independent (the flush replay
** re-descends by rowid), so they stay.
**
** Returns BF_OK, or SQLITE_NOMEM if the scratch page cannot be allocated (in
** which case the mini-page is left untouched).
*/
int sqlite3BfMiniPageDropClean(BfMiniPage *pMini){
  u32 size = pMini->nodeSize;
  u8 *pTemp;
  BfMiniPage *pNew;
  int rc;

  if( pMini->metaCount==0 ) return BF_OK;
  pTemp = sqlite3_malloc((int)size);
  if( !pTemp ) return SQLITE_NOMEM;
  pNew = (BfMiniPage*)pTemp;
  rc = sqlite3BfMiniPageCopy(pNew, (u16)size, pMini, BF_COPY_DIRTY);
  if( rc==BF_OK ){
    pNew->ownerPgno = pMini->ownerPgno;
    pNew->rootPgno  = pMini->rootPgno;
    memcpy(pMini, pNew, size);
  }
  sqlite3_free(pTemp);
  return rc;
}

/*
** Copy a mini-page to a new location with potentially different size.
** Used for upgrading mini-page size class (BF_COPY_ALL), for the eviction
** sweep (BF_COPY_REFERENCED), and to reclaim space inside a full mini-page by
** dropping its pure-cache records (BF_COPY_DIRTY).
**
** BF_COPY_DIRTY keeps only BFOP_INSERT/BFOP_DELETE.  Dropping a BFOP_CACHE
** (duplicate of a base cell) or a BFOP_PHANTOM (confirmed-absent key) costs at
** most a later cache miss, never correctness — but dropping a dirty record
** would lose committed data, so those are always carried over.
*/
int sqlite3BfMiniPageCopy(BfMiniPage *pDst, u16 dstSize,
    BfMiniPage *pSrc, int copyMode){
  BfKVMeta *aSrcMeta = bfMiniPageMeta(pSrc);
  int i;
  u8 *pKey, *pVal;
  int nKey, nVal;
  u8 opType;

  sqlite3BfMiniPageInit(pDst, dstSize, pSrc->baseDiskOffset);
  pDst->ownerPgno = pSrc->ownerPgno;
  pDst->rootPgno = pSrc->rootPgno;

  for(i = 0; i < pSrc->metaCount; i++){
    if( copyMode==BF_COPY_REFERENCED && !BF_KV_IS_REF(&aSrcMeta[i]) ){
      continue;
    }
    if( copyMode==BF_COPY_DIRTY ){
      u8 op = BF_KV_OP_TYPE(&aSrcMeta[i]);
      if( op!=BFOP_INSERT && op!=BFOP_DELETE ) continue;
    }

    pKey = bfGetKeyPtr(pSrc, &aSrcMeta[i]);
    nKey = BF_KV_KEY_LEN(&aSrcMeta[i]);
    pVal = bfGetValuePtr(pSrc, &aSrcMeta[i]);
    nVal = BF_KV_VALUE_LEN(&aSrcMeta[i]);
    opType = BF_KV_OP_TYPE(&aSrcMeta[i]);

    int rc = sqlite3BfMiniPageInsert(pDst, pKey, nKey, pVal, nVal, opType);
    if( rc != BF_OK ) return rc;
  }

  return BF_OK;
}

#endif /* !defined(SQLITE_OMIT_BF_CACHE) */
