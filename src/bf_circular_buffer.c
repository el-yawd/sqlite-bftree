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
** This file implements the circular buffer for Bf-Tree mini-pages.
**
** The circular buffer is a ring buffer that supports variable-length
** allocations. It uses a FIFO eviction policy with a "copy-on-access"
** region near the head to prevent eviction of hot data.
**
** Inspired by FASTER's hybrid log design.
*/
#include "sqliteInt.h"
#ifndef SQLITE_OMIT_BF_CACHE
#include "bf_cache.h"

#ifdef SQLITE_BF_DEBUG
#include <fcntl.h>
#include <unistd.h>
static void bfCbTrace(const char *zTag, const void *pPtr, int nSize){
  char zBuf[256];
  int n = snprintf(zBuf, sizeof(zBuf), "bfcbuf: %s ptr=%p size=%d\n", zTag, pPtr, nSize);
  int fd = open("/home/yawd/Projects/tfg-stuff/sqlite/build/bf-allocs.log",
                O_WRONLY|O_CREAT|O_APPEND, 0644);
  if( fd>=0 ){ write(fd, zBuf, n); close(fd); }
}
#define BF_CB_TRACE(t,p,n)  bfCbTrace(t,p,n)
#else
#define BF_CB_TRACE(t,p,n)  ((void)0)
#endif

/*
** Align size up to 8-byte boundary.
*/
#define BF_ALIGN8(x)  (((x) + 7) & ~7)

/*
** Size of allocation metadata.
*/
#define BF_ALLOC_META_SIZE  sizeof(BfAllocMeta)

/*
** Get the size class index for a given size.
** Returns -1 if size is larger than maximum.
*/
static int bfGetSizeClassIndex(BfCircularBuffer *pCb, u32 size){
  int i;
  for(i = 0; i < BF_SIZE_CLASS_COUNT; i++){
    if( size <= pCb->freeList.aSizeClass[i] ){
      return i;      /* aSizeClass ascends, so this is the smallest that fits */
    }
  }
  return -1;
}

/*
** Initialize size classes for free list: 64, 128, 256, 512, 1024, 2048, 4096,
** ASCENDING, so aSizeClass[0] is the smallest.
**
** This array used to be filled backwards (index 0 = 4096), which every reader
** since has had to know.  Two of them did not: sqlite3BfMiniPageNextSizeClass
** and sqlite3BfMiniPageSizeClassFor walk it with for(i=0;...) and take the first
** class that fits, so on a descending array they returned 4096 every time --
** every mini-page that outgrew its 64-byte allocation jumped straight to 4 KiB.
** That is the "37x space amplification" the 2026-09-07 note describes as fixed:
** the loop was turned around, the array was not, so nothing changed.  One order,
** the obvious one, everywhere.
*/
static void bfInitSizeClasses(BfFreeList *pFl){
  int i;
  u32 size = BF_MIN_MINI_PAGE;
  for(i = 0; i < BF_SIZE_CLASS_COUNT; i++){
    pFl->aSizeClass[i] = size;
    pFl->apHead[i] = 0;
    size *= 2;
  }
  assert( pFl->aSizeClass[0]==BF_MIN_MINI_PAGE );
  assert( pFl->aSizeClass[BF_SIZE_CLASS_COUNT-1]==BF_MAX_MINI_PAGE );
}

/*
** Convert logical address to physical pointer.
** The circular buffer uses logical addresses that wrap around.
*/
static u8 *bfLogicalToPhysical(BfCircularBuffer *pCb, u64 addr){
  u64 offset = addr & (pCb->capacity - 1);
  return pCb->pBuffer + offset;
}

/*
** Get allocation metadata from data pointer.
** The metadata is stored immediately before the allocated data.
*/
static BfAllocMeta *bfGetMetaFromDataPtr(void *pData){
  return (BfAllocMeta*)((u8*)pData - BF_ALLOC_META_SIZE);
}

/*
** Compute distance from a pointer to the tail.
** Used to determine if pointer is in copy-on-access region.
*/
static u64 bfDistanceToTail(BfCircularBuffer *pCb, void *ptr){
  u8 *pPtr = (u8*)ptr;
  u8 *pTail = bfLogicalToPhysical(pCb, pCb->tailAddr);

  if( pTail >= pPtr ){
    return pTail - pPtr;
  }else{
    return pCb->capacity - (pPtr - pTail);
  }
}

/*
** Check if a pointer is in the copy-on-access region.
** If true, the data should be copied to tail before modification.
*/
int sqlite3BfCircularBufferIsCopyOnAccess(BfCircularBuffer *pCb, void *ptr){
  u64 distance = bfDistanceToTail(pCb, ptr);
  return distance >= pCb->copyOnAccessThreshold;
}

/*
** Accessors for BfAllocMeta.state.
**
** The state byte tracks each allocation's lifecycle (NOT_READY → READY →
** BEGIN_TOMBSTONE → TOMBSTONE → EVICTED).  In the embedded engine every BF
** access is serialized by the owning BtShared mutex, so there is no concurrent
** allocator/eviction thread to race against: these are plain loads and stores.
** The state VALUES are still meaningful (the lifecycle is real), only the
** atomics/CAS that guarded the lock-free multi-thread variant are gone.
*/

/* Load the current state. */
static u8 bfMetaLoadState(const BfAllocMeta *pMeta){
  return pMeta->state;
}

/* Store a new state. */
static void bfMetaStoreState(BfAllocMeta *pMeta, u8 newState){
  pMeta->state = newState;
}

/*
** Transition state from READY to BEGIN_TOMBSTONE.
** Returns 1 on success, 0 if state was not READY.  Serialized by BtShared.
*/
static int bfTryBeginTombstone(BfAllocMeta *pMeta){
  if( pMeta->state == BF_STATE_READY ){
    pMeta->state = BF_STATE_BEGIN_TOMBSTONE;
    return 1;
  }
  return 0;
}

/* BEGIN_TOMBSTONE → TOMBSTONE */
static void bfToTombstone(BfAllocMeta *pMeta){
  assert( bfMetaLoadState(pMeta) == BF_STATE_BEGIN_TOMBSTONE );
  bfMetaStoreState(pMeta, BF_STATE_TOMBSTONE);
}

/* TOMBSTONE → EVICTED */
static void bfTombstoneToEvicted(BfAllocMeta *pMeta){
  assert( bfMetaLoadState(pMeta) == BF_STATE_TOMBSTONE );
  bfMetaStoreState(pMeta, BF_STATE_EVICTED);
}

/* NOT_READY → READY */
static void bfToReady(BfAllocMeta *pMeta){
  assert( bfMetaLoadState(pMeta) == BF_STATE_NOT_READY );
  bfMetaStoreState(pMeta, BF_STATE_READY);
}

/* BEGIN_TOMBSTONE → READY (rollback) */
static void bfRevertToReady(BfAllocMeta *pMeta){
  assert( bfMetaLoadState(pMeta) == BF_STATE_BEGIN_TOMBSTONE );
  bfMetaStoreState(pMeta, BF_STATE_READY);
}

/*
** Try to add a pointer to the free list.
** Returns 1 on success, 0 if list is locked.
*/
static int bfFreeListAdd(BfFreeList *pFl, void *ptr, u32 size){
  int idx = -1;
  int i;
  void **ppNext;

  /* Smallest class that fits (aSizeClass ascends). */
  for(i = 0; i < BF_SIZE_CLASS_COUNT; i++){
    if( size <= pFl->aSizeClass[i] ){
      idx = i;
      break;
    }
  }
  if( idx < 0 ) return 0;

  sqlite3_mutex_enter(pFl->mutex);

  /* Store next pointer in the freed memory itself */
  ppNext = (void**)ptr;
  *ppNext = pFl->apHead[idx];
  pFl->apHead[idx] = ptr;

  sqlite3_mutex_leave(pFl->mutex);
  BF_CB_TRACE("freelist-add", ptr, (int)size);
  return 1;
}

/*
** Try to remove a pointer from the free list for a given size.
** Returns pointer on success, NULL if list is empty.
*/
static void *bfFreeListRemove(BfFreeList *pFl, u32 size){
  int idx = -1;
  int i;
  void *ptr;
  void **ppNext;

  /* Smallest class that fits AND has a free block (aSizeClass ascends). */
  for(i = 0; i < BF_SIZE_CLASS_COUNT; i++){
    if( size <= pFl->aSizeClass[i] && pFl->apHead[i] ){
      idx = i;
      break;
    }
  }
  if( idx < 0 ) return 0;

  sqlite3_mutex_enter(pFl->mutex);

  ptr = pFl->apHead[idx];
  if( ptr ){
    /* The chain lives INSIDE the freed blocks, so it is only as valid as the
    ** blocks are.  A block can be put on this list and later be swept by the
    ** FIFO eviction head; once the tail wraps around and reuses that address,
    ** its first 8 bytes are payload, and following the "next" pointer walks
    ** into whatever now occupies them.
    **
    ** Nothing removes a block from this list when the head passes it, so
    ** validate on the way out: a block that is genuinely still free is still in
    ** BF_STATE_FREELISTED.  Anything else means the chain has been overtaken,
    ** and since the next pointer is no longer trustworthy either, the whole
    ** class is abandoned rather than followed.  The space is not lost -- it is
    ** reclaimed by the head sweep like any other block.
    **
    ** This was latent for as long as eviction never actually ran (every read
    ** benchmark here reports evictions=0).  It segfaults within seconds of the
    ** ring genuinely cycling. */
    BfAllocMeta *pMeta = bfGetMetaFromDataPtr(ptr);
    if( bfMetaLoadState(pMeta)!=BF_STATE_FREELISTED ){
      pFl->apHead[idx] = 0;
      ptr = 0;
      BF_CB_TRACE("freelist-stale-chain-dropped", 0, (int)size);
    }else{
      ppNext = (void**)ptr;
      pFl->apHead[idx] = *ppNext;
      BF_CB_TRACE("freelist-remove", ptr, (int)size);
    }
  }

  sqlite3_mutex_leave(pFl->mutex);
  return ptr;
}

/*
** Initialize a circular buffer with the given capacity.
** Capacity must be a power of 2.
*/
int sqlite3BfCircularBufferInit(BfCircularBuffer *pCb, u64 capacity){
  assert( (capacity & (capacity - 1)) == 0 );  /* Must be power of 2 */
  assert( capacity >= BF_MAX_MINI_PAGE + BF_ALLOC_META_SIZE );

  memset(pCb, 0, sizeof(*pCb));

  pCb->pBuffer = (u8*)sqlite3_malloc64(capacity);
  if( !pCb->pBuffer ){
    BF_CB_TRACE("cbuf-malloc-failed", NULL, (int)capacity);
    return SQLITE_NOMEM;
  }

  pCb->capacity = capacity;
  pCb->headAddr = 0;
  pCb->tailAddr = 0;
  pCb->evictingAddr = 0;

  pCb->mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
  if( !pCb->mutex ){
    BF_CB_TRACE("cbuf-mutex-failed", pCb->pBuffer, (int)capacity);
    sqlite3_free(pCb->pBuffer);
    pCb->pBuffer = 0;
    return SQLITE_NOMEM;
  }

  /* Initialize free list */
  pCb->freeList.mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
  if( !pCb->freeList.mutex ){
    BF_CB_TRACE("cbuf-freelist-mutex-failed", pCb->pBuffer, (int)capacity);
    sqlite3_mutex_free(pCb->mutex);
    sqlite3_free(pCb->pBuffer);
    return SQLITE_NOMEM;
  }
  bfInitSizeClasses(&pCb->freeList);

  /* Configure copy-on-access threshold */
  pCb->copyOnAccessRatio = BF_DEFAULT_COPY_ON_ACCESS;
  pCb->copyOnAccessThreshold = (u64)(capacity * (1.0 - pCb->copyOnAccessRatio));
  BF_CB_TRACE("cbuf-init", pCb->pBuffer, (int)capacity);

  return SQLITE_OK;
}

/*
** Destroy a circular buffer and free all resources.
*/
void sqlite3BfCircularBufferDestroy(BfCircularBuffer *pCb){
  if( pCb->pBuffer ){
    BF_CB_TRACE("cbuf-destroy", pCb->pBuffer, (int)pCb->capacity);
    sqlite3_free(pCb->pBuffer);
    pCb->pBuffer = 0;
  }
  if( pCb->mutex ){
    sqlite3_mutex_free(pCb->mutex);
    pCb->mutex = 0;
  }
  if( pCb->freeList.mutex ){
    sqlite3_mutex_free(pCb->freeList.mutex);
    pCb->freeList.mutex = 0;
  }
}

/*
** Allocate memory from the circular buffer.
** Returns pointer to allocated memory, or NULL if buffer is full.
**
** The allocated memory is initially in NOT_READY state. The caller
** must call sqlite3BfCircularBufferMarkReady() after initializing the data.
*/
void *sqlite3BfCircularBufferAlloc(BfCircularBuffer *pCb, u32 size){
  u32 alignedSize;
  u32 required;
  u64 logicalRemaining;
  u64 physicalRemaining;
  BfAllocMeta *pMeta;
  void *ptr;
  u8 *pPhysical;

  if( size == 0 ) return 0;
  if( size < BF_MIN_MINI_PAGE ) size = BF_MIN_MINI_PAGE;

  sqlite3_mutex_enter(pCb->mutex);

  /* Try free list first */
  ptr = bfFreeListRemove(&pCb->freeList, size);
  if( ptr ){
    pMeta = bfGetMetaFromDataPtr(ptr);

    /* Check if in copy-on-access region */
    if( sqlite3BfCircularBufferIsCopyOnAccess(pCb, ptr) ){
      /* Mark as tombstone and retry from tail */
      bfMetaStoreState(pMeta, BF_STATE_TOMBSTONE);
    }else{
      /* Reuse from free list */
      bfMetaStoreState(pMeta, BF_STATE_NOT_READY);
      pCb->nFreeListHits++;
      BF_CB_TRACE("alloc-freelist-hit", ptr, (int)size);
      sqlite3_mutex_leave(pCb->mutex);
      return ptr;
    }
  }

  /* Calculate space needed */
  alignedSize = BF_ALIGN8(size);
  required = alignedSize + BF_ALLOC_META_SIZE;

  /* Check if enough logical space */
  logicalRemaining = pCb->capacity - (pCb->tailAddr - pCb->headAddr);
  if( logicalRemaining < required ){
    sqlite3_mutex_leave(pCb->mutex);
    return 0;  /* Buffer full */
  }

  /* Check if enough contiguous physical space */
  physicalRemaining = pCb->capacity - (pCb->tailAddr & (pCb->capacity - 1));
  if( physicalRemaining < required ){
    /* Not enough contiguous space - insert tombstone for remaining */
    if( physicalRemaining >= BF_ALLOC_META_SIZE ){
      pPhysical = bfLogicalToPhysical(pCb, pCb->tailAddr);
      pMeta = (BfAllocMeta*)pPhysical;
      pMeta->size = (u32)(physicalRemaining - BF_ALLOC_META_SIZE);
      bfMetaStoreState(pMeta, BF_STATE_TOMBSTONE);
      pCb->tailAddr += physicalRemaining;
    }
    sqlite3_mutex_leave(pCb->mutex);
    /* Recursive call will allocate from buffer start */
    return sqlite3BfCircularBufferAlloc(pCb, size);
  }

  /* Allocate from tail */
  pPhysical = bfLogicalToPhysical(pCb, pCb->tailAddr);
  pMeta = (BfAllocMeta*)pPhysical;
  pMeta->size = alignedSize;
  bfMetaStoreState(pMeta, BF_STATE_NOT_READY);

  ptr = pPhysical + BF_ALLOC_META_SIZE;
  pCb->tailAddr += required;
  pCb->nAllocs++;
  BF_CB_TRACE("alloc-tail", ptr, (int)size);

  sqlite3_mutex_leave(pCb->mutex);
  return ptr;
}

/*
** Mark allocated memory as ready for use.
** Call this after initializing the allocated data.
*/
void sqlite3BfCircularBufferMarkReady(void *ptr){
  BfAllocMeta *pMeta = bfGetMetaFromDataPtr(ptr);
  bfToReady(pMeta);
}

/*
** Deallocate memory back to the circular buffer.
** The memory is added to the free list for reuse.
**
** Returns BF_OK on success.
*/
int sqlite3BfCircularBufferDealloc(BfCircularBuffer *pCb, void *ptr){
  BfAllocMeta *pMeta;

  if( !ptr ) return BF_OK;

  pMeta = bfGetMetaFromDataPtr(ptr);

  /* Atomically acquire exclusive dealloc handle (READY → BEGIN_TOMBSTONE). */
  if( !bfTryBeginTombstone(pMeta) ){
    /* State was not READY — already being deallocated or not yet ready. */
    return BF_ERROR;
  }

  /* If in copy-on-access region, just mark as tombstone */
  if( sqlite3BfCircularBufferIsCopyOnAccess(pCb, ptr) ){
    bfToTombstone(pMeta);
    BF_CB_TRACE("dealloc-copy-on-access", ptr, (int)pMeta->size);
    return BF_OK;
  }

  /* Try to add to free list */
  if( bfFreeListAdd(&pCb->freeList, ptr, pMeta->size) ){
    bfMetaStoreState(pMeta, BF_STATE_FREELISTED);
  }else{
    bfToTombstone(pMeta);
  }
  BF_CB_TRACE("dealloc", ptr, (int)pMeta->size);

  return BF_OK;
}

/*
** Try to acquire exclusive handle for deallocation.
** Returns pointer on success, NULL if contention.
*/
void *sqlite3BfCircularBufferAcquireDeallocHandle(BfCircularBuffer *pCb, void *ptr){
  BfAllocMeta *pMeta = bfGetMetaFromDataPtr(ptr);
  (void)pCb;

  if( bfTryBeginTombstone(pMeta) ){
    return ptr;
  }
  return 0;
}

/*
** Release dealloc handle without deallocating.
*/
void sqlite3BfCircularBufferReleaseDeallocHandle(void *ptr){
  BfAllocMeta *pMeta = bfGetMetaFromDataPtr(ptr);
  bfRevertToReady(pMeta);
}

/*
** Finish deallocation after acquiring handle.
*/
void sqlite3BfCircularBufferFinishDealloc(BfCircularBuffer *pCb, void *ptr, int addToFreeList){
  BfAllocMeta *pMeta = bfGetMetaFromDataPtr(ptr);

  if( !addToFreeList || sqlite3BfCircularBufferIsCopyOnAccess(pCb, ptr) ){
    bfToTombstone(pMeta);
    BF_CB_TRACE("finish-dealloc-tombstone", ptr, (int)pMeta->size);
    return;
  }

  if( bfFreeListAdd(&pCb->freeList, ptr, pMeta->size) ){
    bfMetaStoreState(pMeta, BF_STATE_FREELISTED);
  }else{
    bfToTombstone(pMeta);
  }
  BF_CB_TRACE("finish-dealloc", ptr, (int)pMeta->size);
}

/*
** Try to bump head address to evicting address.
** Returns number of bytes advanced.
*/
static u32 bfTryBumpHead(BfCircularBuffer *pCb){
  u64 headAddr = pCb->headAddr;
  u64 oldAddr = headAddr;
  BfAllocMeta *pMeta;
  u32 advance;

  while( headAddr < pCb->evictingAddr ){
    pMeta = (BfAllocMeta*)bfLogicalToPhysical(pCb, headAddr);
    if( bfMetaLoadState(pMeta) != BF_STATE_EVICTED ){
      break;
    }
    advance = pMeta->size + BF_ALLOC_META_SIZE;
    pCb->headAddr += advance;
    headAddr += advance;
  }

  return (u32)(headAddr - oldAddr);
}

/*
** Evict one entry from the circular buffer.
** Calls the eviction callback to handle the data before eviction.
**
** Returns number of bytes advanced, or 0 if buffer is empty.
*/
int sqlite3BfCircularBufferEvictOne(BfCircularBuffer *pCb,
    int (*xEvict)(void*, void*), void *pCtx){
  u64 startAddr, endAddr;
  BfAllocMeta *pMeta;
  u8 *pData;
  void *pHandle;
  int rc;

  sqlite3_mutex_enter(pCb->mutex);

  /* Check if anything to evict */
  if( pCb->evictingAddr >= pCb->tailAddr ){
    sqlite3_mutex_leave(pCb->mutex);
    return 0;
  }

  /* Reserve the next entry for eviction */
  startAddr = pCb->evictingAddr;
  pMeta = (BfAllocMeta*)bfLogicalToPhysical(pCb, startAddr);
  endAddr = startAddr + pMeta->size + BF_ALLOC_META_SIZE;
  pCb->evictingAddr = endAddr;

  sqlite3_mutex_leave(pCb->mutex);

  /* Get data pointer */
  pData = ((u8*)pMeta) + BF_ALLOC_META_SIZE;

  /* Try to acquire exclusive handle */
  for(;;){
    pHandle = sqlite3BfCircularBufferAcquireDeallocHandle(pCb, pData);
    if( pHandle ){
      /* Call eviction callback */
      rc = xEvict(pCtx, pHandle);
      if( rc == BF_OK ){
        bfToTombstone(pMeta);
        bfTombstoneToEvicted(pMeta);
        break;
      }else{
        /* Callback refused the eviction (e.g. a dirty mini-page whose
        ** buffered writes have not been applied to the base pages).  FIFO
        ** order means nothing behind this entry can advance either: revert
        ** the reservation and abort the sweep instead of retrying forever. */
        sqlite3BfCircularBufferReleaseDeallocHandle(pHandle);
        sqlite3_mutex_enter(pCb->mutex);
        if( pCb->evictingAddr == endAddr ){
          pCb->evictingAddr = startAddr;
        }
        sqlite3_mutex_leave(pCb->mutex);
        return -1;
      }
    }else{
      /* Could not acquire the dealloc handle via CAS.  Read the current
      ** state atomically and decide how to proceed. */
      u8 st = bfMetaLoadState(pMeta);
      if( st == BF_STATE_TOMBSTONE ){
        /* Already tombstoned (e.g. copy-on-access path); advance. */
        bfTombstoneToEvicted(pMeta);
        break;
      }else if( st == BF_STATE_EVICTED ){
        /* Another eviction thread completed this entry already. */
        break;
      }else if( st == BF_STATE_FREELISTED ){
        /* The allocation is sitting on the free list.  A FREELISTED entry
        ** can never be acquired via bfTryBeginTombstone because its state
        ** is not READY, which would spin forever.
        **
        ** Transition it directly to TOMBSTONE → EVICTED so the eviction
        ** sweep can advance headAddr.  The dangling free-list pointer is
        ** benign: bfFreeListRemove will return it, but the allocator checks
        ** copy-on-access status before reusing, and a double-use of an
        ** evicted slot just results in a fresh tail allocation instead. */
        bfMetaStoreState(pMeta, BF_STATE_TOMBSTONE);
        bfTombstoneToEvicted(pMeta);
        break;
      }else if( st == BF_STATE_NOT_READY ){
        /* Being initialised by another thread; wait briefly and retry. */
        sqlite3_sleep(1);
      }else{
        /* BEGIN_TOMBSTONE: another thread holds the dealloc handle; retry. */
        sqlite3_sleep(1);
      }
    }
  }

  /* Try to advance head */
  sqlite3_mutex_enter(pCb->mutex);
  bfTryBumpHead(pCb);
  pCb->nEvictions++;
  sqlite3_mutex_leave(pCb->mutex);

  return (int)(endAddr - startAddr);
}

/*
** Evict up to n entries from the circular buffer.
** Returns total bytes evicted.
*/
int sqlite3BfCircularBufferEvictN(BfCircularBuffer *pCb, int n,
    int (*xEvict)(void*, void*), void *pCtx){
  int i;
  int total = 0;
  int evicted;

  for(i = 0; i < n; i++){
    evicted = sqlite3BfCircularBufferEvictOne(pCb, xEvict, pCtx);
    if( evicted <= 0 ) break;  /* 0 = buffer empty, -1 = sweep refused/aborted */
    total += evicted;
  }

  return total;
}

/*
** Get the allocated size for a pointer.
*/
u32 sqlite3BfCircularBufferGetSize(void *ptr){
  BfAllocMeta *pMeta = bfGetMetaFromDataPtr(ptr);
  return pMeta->size;
}

/*
** Get buffer usage statistics.
*/
void sqlite3BfCircularBufferStats(BfCircularBuffer *pCb,
    u64 *pUsed, u64 *pCapacity, u64 *pAllocs, u64 *pEvictions){
  sqlite3_mutex_enter(pCb->mutex);
  if( pUsed ) *pUsed = pCb->tailAddr - pCb->headAddr;
  if( pCapacity ) *pCapacity = pCb->capacity;
  if( pAllocs ) *pAllocs = pCb->nAllocs;
  if( pEvictions ) *pEvictions = pCb->nEvictions;
  sqlite3_mutex_leave(pCb->mutex);
}

#endif /* !defined(SQLITE_OMIT_BF_CACHE) */
