/*
** 2026 — BF-Tree v2, Phase 2 (physiological WAL).
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Record-batch frame codec for the BF-Tree physiological WAL.  See bf_wal.h
** for the format and the invariants.  This translation unit is PURE: it
** touches only the caller-supplied byte buffers and fixed-width integers, so
** it can be model-checked (ESBMC) and fuzzed (libFuzzer) in isolation.
**
** Payload layout (all multi-byte scalars big-endian, to match SQLite's WAL):
**
**   offset 0 : u32 magic   (BFWAL_MAGIC)
**   offset 4 : u16 version  (BFWAL_VERSION)
**   offset 6 : u16 nRec     (record count)
**   offset 8 : u32 nUsed    (total valid bytes, header included)
**   offset 12: records, each
**              u32 pgno | u8 op | varint nKey | varint nVal | key | val
**   ...      : zero padding to szBuf
**
** Lengths use an unsigned LEB128 varint (self-contained, NOT sqlite3's varint)
** so the module has no dependency on the rest of the tree.
*/
/* BF_WAL_STANDALONE lets a unit test / ESBMC / libFuzzer harness #include this
** translation unit directly, supplying its own fixed-width typedefs and string
** helpers, without dragging in the amalgamation.  In the normal build it is
** undefined and the module compiles as part of sqlite3.c. */
#ifndef BF_WAL_STANDALONE
#include "sqliteInt.h"
#endif
#ifndef SQLITE_OMIT_BF_CACHE
#include "bf_wal.h"

/* Allocation seam: the amalgamation build routes through sqlite3's allocator;
** a standalone harness supplies libc's (see BF_WAL_STANDALONE in bf_wal.h). */
#ifdef BF_WAL_STANDALONE
# include <stdlib.h>
# include <string.h>
# define BFWAL_MALLOC(n)     malloc((size_t)(n))
# define BFWAL_REALLOC(p,n)  realloc((p),(size_t)(n))
# define BFWAL_FREE(p)       free(p)
#else
# define BFWAL_MALLOC(n)     sqlite3_malloc((int)(n))
# define BFWAL_REALLOC(p,n)  sqlite3_realloc((p),(int)(n))
# define BFWAL_FREE(p)       sqlite3_free(p)
#endif

/* Largest LEB128 encoding of a u32 is 5 bytes (32 bits / 7 per byte). */
#define BFWAL_VARINT_MAX 5

/*
** Fixed-width big-endian scalar helpers.  Kept local (not sqlite3Get4byte)
** so the codec stands alone under a bounded model checker.
*/
static void bfPut16(u8 *p, u32 v){
  p[0] = (u8)(v>>8); p[1] = (u8)v;
}
static void bfPut32(u8 *p, u32 v){
  p[0] = (u8)(v>>24); p[1] = (u8)(v>>16); p[2] = (u8)(v>>8); p[3] = (u8)v;
}
static u32 bfGet16(const u8 *p){
  return ((u32)p[0]<<8) | (u32)p[1];
}
static u32 bfGet32(const u8 *p){
  return ((u32)p[0]<<24) | ((u32)p[1]<<16) | ((u32)p[2]<<8) | (u32)p[3];
}

/*
** Encode v as LEB128 into p (which has at least BFWAL_VARINT_MAX bytes).
** Returns the number of bytes written (1..5).
*/
static int bfPutVarint(u8 *p, u32 v){
  int i = 0;
  while( v>=0x80 ){
    p[i++] = (u8)(v | 0x80);
    v >>= 7;
  }
  p[i++] = (u8)v;
  return i;
}

/* Encoded length of v as a LEB128 varint (1..5). */
static int bfVarintLen(u32 v){
  int n = 1;
  while( v>=0x80 ){ v >>= 7; n++; }
  return n;
}

/*
** Decode a LEB128 varint from p[0..avail-1] into *pv.  Returns the number of
** bytes consumed (1..5) on success, or -1 if the value is truncated (runs off
** the end of the available bytes) or overflows a u32.  Never reads past avail.
*/
static int bfGetVarint(const u8 *p, int avail, u32 *pv){
  u32 v = 0;
  int i = 0, shift = 0;
  while( i<avail && i<BFWAL_VARINT_MAX ){
    u8 c = p[i++];
    /* A 5th byte contributes only bits 28..31; higher bits would overflow. */
    if( shift==28 && (c&0xF0)!=0 ) return -1;
    v |= ((u32)(c & 0x7F)) << shift;
    if( (c & 0x80)==0 ){ *pv = v; return i; }
    shift += 7;
  }
  return -1;   /* truncated, or would need a 6th continuation byte */
}

/* Encoded byte cost of a single record. */
int sqlite3BfWalRecSize(const BfWalRec *pRec){
  return 4                              /* leaf pgno */
       + 4                              /* root pgno */
       + 1                              /* op        */
       + bfVarintLen(pRec->nKey)
       + bfVarintLen(pRec->nVal)
       + (int)pRec->nKey
       + (int)pRec->nVal;
}

/*
** Encoder.
*/
void sqlite3BfWalBatchInit(BfWalBatch *p, u8 *aBuf, int szBuf){
  p->aBuf  = aBuf;
  p->szBuf = szBuf;
  p->nUsed = BFWAL_HDRSIZE;
  p->nRec  = 0;
}

/*
** Append one record.  Returns BFWAL_OK, or BFWAL_FULL if the record will not
** fit (the batch is left unchanged so the caller can finish and flush it, then
** start a fresh frame).  Rejects malformed callers (nRec/nKey overflow) as
** BFWAL_FULL as well so a batch never encodes something the decoder must
** treat as corrupt.
*/
int sqlite3BfWalBatchAppend(BfWalBatch *p, const BfWalRec *pRec){
  int need;
  u8 *q;

  /* nRec is a u16 field; keyLen must fit the mini-page 14-bit key field. */
  if( p->nRec>=0xFFFF ) return BFWAL_FULL;
  if( pRec->pgno==0 || pRec->rootPgno<=1 ) return BFWAL_FULL;
  if( pRec->nKey>0x3FFF ) return BFWAL_FULL;
  if( pRec->op==BFWAL_OP_MERGED && (pRec->nKey!=16 || pRec->nVal!=8) ){
    return BFWAL_FULL;      /* would not decode: refuse to encode it */
  }
  if( pRec->op!=BFWAL_OP_INSERT && pRec->op!=BFWAL_OP_DELETE
   && pRec->op!=BFWAL_OP_MERGED ){
    return BFWAL_FULL;
  }

  need = sqlite3BfWalRecSize(pRec);
  if( p->nUsed > p->szBuf - need ) return BFWAL_FULL;

  q = p->aBuf + p->nUsed;
  bfPut32(q, pRec->pgno);        q += 4;
  bfPut32(q, pRec->rootPgno);    q += 4;
  *q++ = pRec->op;
  q += bfPutVarint(q, pRec->nKey);
  q += bfPutVarint(q, pRec->nVal);
  if( pRec->nKey ){ memcpy(q, pRec->pKey, pRec->nKey); q += pRec->nKey; }
  if( pRec->nVal ){ memcpy(q, pRec->pVal, pRec->nVal); q += pRec->nVal; }

  p->nUsed = (int)(q - p->aBuf);
  p->nRec++;
  return BFWAL_OK;
}

/*
** Finalise the batch: write the header and zero-fill the tail so the whole
** szBuf payload is deterministic (important for the WAL checksum chain).
*/
void sqlite3BfWalBatchFinish(BfWalBatch *p){
  bfPut32(p->aBuf + 0, BFWAL_MAGIC);
  bfPut16(p->aBuf + 4, BFWAL_VERSION);
  bfPut16(p->aBuf + 6, (u32)p->nRec);
  bfPut32(p->aBuf + 8, (u32)p->nUsed);
  if( p->nUsed < p->szBuf ){
    memset(p->aBuf + p->nUsed, 0, (size_t)(p->szBuf - p->nUsed));
  }
}

/*
** Validate and open an iterator over a frame payload.  Returns BFWAL_OK if the
** header is well-formed, else BFWAL_CORRUPT.  Does not scan the records; each
** is validated lazily by IterNext.
*/
int sqlite3BfWalIterInit(BfWalIter *it, const u8 *aBuf, int szBuf){
  u32 nUsed, nRec;

  memset(it, 0, sizeof(*it));
  if( szBuf < BFWAL_HDRSIZE ) return BFWAL_CORRUPT;
  if( bfGet32(aBuf + 0) != BFWAL_MAGIC ) return BFWAL_CORRUPT;
  if( bfGet16(aBuf + 4) != BFWAL_VERSION ) return BFWAL_CORRUPT;
  nRec  = bfGet16(aBuf + 6);
  nUsed = bfGet32(aBuf + 8);
  /* nUsed must lie within the buffer and cover at least the header. */
  if( nUsed < (u32)BFWAL_HDRSIZE || nUsed > (u32)szBuf ) return BFWAL_CORRUPT;

  it->aBuf = aBuf;
  it->nUsed = (int)nUsed;
  it->nRec = (int)nRec;
  it->off = BFWAL_HDRSIZE;
  it->iRec = 0;
  return BFWAL_OK;
}

/*
** Yield the next record.  Returns BFWAL_OK and fills *pRec, BFWAL_DONE when the
** advertised record count is exhausted, or BFWAL_CORRUPT if a record is
** malformed or would extend past nUsed.  All bounds are checked against nUsed,
** never szBuf, and the routine never reads outside [aBuf, aBuf+nUsed).
*/
int sqlite3BfWalIterNext(BfWalIter *it, BfWalRec *pRec){
  const u8 *p;
  int off, avail, n;
  u32 pgno, rootPgno, nKey, nVal;
  u8 op;

  if( it->iRec >= it->nRec ) return BFWAL_DONE;

  off = it->off;
  avail = it->nUsed - off;
  if( avail < 9 ) return BFWAL_CORRUPT; /* leaf(4) + root(4) + op(1) */
  p = it->aBuf + off;

  pgno = bfGet32(p);      p += 4;  off += 4;
  rootPgno = bfGet32(p);  p += 4;  off += 4;
  op = *p++;              off += 1;
  if( pgno==0 || rootPgno<=1 ) return BFWAL_CORRUPT;
  if( op!=BFWAL_OP_INSERT && op!=BFWAL_OP_DELETE && op!=BFWAL_OP_MERGED ){
    return BFWAL_CORRUPT;
  }

  n = bfGetVarint(p, it->nUsed - off, &nKey);
  if( n<0 ) return BFWAL_CORRUPT;
  p += n; off += n;
  n = bfGetVarint(p, it->nUsed - off, &nVal);
  if( n<0 ) return BFWAL_CORRUPT;
  p += n; off += n;

  /* Key/value bodies must fit in the remaining valid bytes.  Guard the
  ** addition against u32 wrap before comparing to the (bounded) remainder. */
  if( nKey > (u32)(it->nUsed - off) ) return BFWAL_CORRUPT;
  off += (int)nKey;
  if( nVal > (u32)(it->nUsed - off) ) return BFWAL_CORRUPT;
  if( op==BFWAL_OP_MERGED && (nKey!=16 || nVal!=8) ) return BFWAL_CORRUPT;

  pRec->pgno = pgno;
  pRec->rootPgno = rootPgno;
  pRec->iFrame = 0;
  pRec->op   = op;
  pRec->nKey = nKey;
  pRec->nVal = nVal;
  pRec->pKey = p;
  pRec->pVal = p + nKey;
  if( nKey==0 ) pRec->pKey = 0;
  if( nVal==0 ) pRec->pVal = 0;

  it->off = off + (int)nVal;
  it->iRec++;
  return BFWAL_OK;
}

/****************************************************************************
** In-memory LOGICAL op index (see bf_wal.h), format v5 (2026-10-18, C3).
**
** Ops are keyed by (table root, 8-byte rowid key); each key node keeps its
** ops in log order.  MERGED markers are kept per index in log order.  Every
** op and marker carries a sequence number in append (= log) order, so "this
** marker comes after that op" is a comparison.  The leaf page number of an op
** is only a hint: replay places a key by descending the tree (C3, P2).
****************************************************************************/
typedef struct BfKeyOp BfKeyOp;
struct BfKeyOp {
  u32  iFrame;              /* WAL frame that carried it */
  u32  iSeq;                /* append order */
  u32  hint;                /* leaf page the writer used (hint only) */
  u8   op;                  /* BFWAL_OP_INSERT / BFWAL_OP_DELETE */
  u32  nVal;
  u8  *pVal;                /* owned (NULL when nVal==0) */
};
typedef struct BfKeyNode BfKeyNode;
struct BfKeyNode {
  u32        root;
  u8         aKey[8];
  int        nOp, nAlloc;
  BfKeyOp   *aOp;           /* log order */
  BfKeyNode *pNext;         /* bucket chain */
};
typedef struct BfMarker BfMarker;
struct BfMarker {
  u32 root;
  u8  lo[8], hi[8];         /* encoded rowids, memcmp order = numeric order */
  u32 snap, start;          /* ops in frames (snap, start) are NOT covered */
  u32 iFrame, iSeq;
  u32 hint;
};
struct BfWalIndex {
  BfKeyNode **apBucket;     /* nBucket chains (power of two) */
  int         nBucket;
  int         nKey;         /* live key nodes */
  BfMarker   *aMark;        /* log order */
  int         nMark, nMarkAlloc;
  u32         iSeq;         /* next sequence number */
};

#define BFWAL_IDX_INIT_BUCKETS 64

static unsigned bfIdxHash(u32 root, const u8 *k, int nBucket){
  u32 h = root * 2654435761u;
  int i;
  for(i=0; i<8; i++) h = (h ^ k[i]) * 16777619u;
  return (unsigned)(h ^ (h>>15)) & (unsigned)(nBucket - 1);
}

static BfKeyNode *bfIdxFind(BfWalIndex *p, u32 root, const u8 *k){
  BfKeyNode *pN = p->apBucket[bfIdxHash(root, k, p->nBucket)];
  for(; pN; pN = pN->pNext){
    if( pN->root==root && memcmp(pN->aKey, k, 8)==0 ) return pN;
  }
  return 0;
}

static int bfIdxMaybeGrow(BfWalIndex *p){
  int newN, i;
  BfKeyNode **apNew;
  if( p->nKey <= (p->nBucket*3)/4 ) return BFWAL_OK;
  newN = p->nBucket * 2;
  apNew = (BfKeyNode**)BFWAL_MALLOC((i64)newN * (int)sizeof(*apNew));
  if( apNew==0 ) return BFWAL_NOMEM;   /* keep running at the old size */
  memset(apNew, 0, (size_t)newN * sizeof(*apNew));
  for(i=0; i<p->nBucket; i++){
    BfKeyNode *pN = p->apBucket[i];
    while( pN ){
      BfKeyNode *pNext = pN->pNext;
      unsigned h = bfIdxHash(pN->root, pN->aKey, newN);
      pN->pNext = apNew[h];
      apNew[h] = pN;
      pN = pNext;
    }
  }
  BFWAL_FREE(p->apBucket);
  p->apBucket = apNew;
  p->nBucket = newN;
  return BFWAL_OK;
}

BfWalIndex *sqlite3BfWalIndexNew(void){
  BfWalIndex *p = (BfWalIndex*)BFWAL_MALLOC((int)sizeof(*p));
  if( p==0 ) return 0;
  memset(p, 0, sizeof(*p));
  p->nBucket = BFWAL_IDX_INIT_BUCKETS;
  p->apBucket = (BfKeyNode**)BFWAL_MALLOC(p->nBucket * (int)sizeof(BfKeyNode*));
  if( p->apBucket==0 ){ BFWAL_FREE(p); return 0; }
  memset(p->apBucket, 0, (size_t)p->nBucket * sizeof(BfKeyNode*));
  return p;
}

static void bfIdxFreeNode(BfKeyNode *pN){
  int i;
  for(i=0; i<pN->nOp; i++) BFWAL_FREE(pN->aOp[i].pVal);
  BFWAL_FREE(pN->aOp);
  BFWAL_FREE(pN);
}

void sqlite3BfWalIndexFree(BfWalIndex *p){
  int i;
  if( p==0 ) return;
  for(i=0; i<p->nBucket; i++){
    BfKeyNode *pN = p->apBucket[i];
    while( pN ){ BfKeyNode *pNext = pN->pNext; bfIdxFreeNode(pN); pN = pNext; }
  }
  BFWAL_FREE(p->apBucket);
  BFWAL_FREE(p->aMark);
  BFWAL_FREE(p);
}

static int bfIdxAddMarker(BfWalIndex *p, const BfWalRec *pRec, u32 iFrame){
  BfMarker *m;
  if( p->nMark>=p->nMarkAlloc ){
    int nNew = p->nMarkAlloc ? p->nMarkAlloc*2 : 16;
    BfMarker *aNew = (BfMarker*)BFWAL_REALLOC(p->aMark, nNew*(int)sizeof(BfMarker));
    if( aNew==0 ) return BFWAL_NOMEM;
    p->aMark = aNew;
    p->nMarkAlloc = nNew;
  }
  m = &p->aMark[p->nMark++];
  m->root = pRec->rootPgno;
  memcpy(m->lo, pRec->pKey, 8);
  memcpy(m->hi, pRec->pKey+8, 8);
  m->snap = bfGet32(pRec->pVal);
  m->start = bfGet32(pRec->pVal+4);
  m->iFrame = iFrame;
  m->iSeq = p->iSeq++;
  m->hint = pRec->pgno;
  return BFWAL_OK;
}

int sqlite3BfWalIndexAppend(BfWalIndex *p, const BfWalRec *pRec, u32 iFrame){
  BfKeyNode *pN;
  BfKeyOp *pOp;
  if( pRec->pgno==0 || pRec->rootPgno<=1 ) return BFWAL_CORRUPT;
  if( pRec->op==BFWAL_OP_MERGED ){
    if( pRec->nKey!=16 || pRec->nVal!=8 ) return BFWAL_CORRUPT;
    return bfIdxAddMarker(p, pRec, iFrame);
  }
  if( pRec->op!=BFWAL_OP_INSERT && pRec->op!=BFWAL_OP_DELETE ) return BFWAL_CORRUPT;
  if( pRec->nKey!=8 ) return BFWAL_CORRUPT;   /* BF buffers rowid keys only */

  pN = bfIdxFind(p, pRec->rootPgno, pRec->pKey);
  if( pN==0 ){
    if( bfIdxMaybeGrow(p) ){ /* NOMEM: proceed at current size, still correct */ }
    pN = (BfKeyNode*)BFWAL_MALLOC((int)sizeof(*pN));
    if( pN==0 ) return BFWAL_NOMEM;
    memset(pN, 0, sizeof(*pN));
    pN->root = pRec->rootPgno;
    memcpy(pN->aKey, pRec->pKey, 8);
    { unsigned h = bfIdxHash(pN->root, pN->aKey, p->nBucket);
      pN->pNext = p->apBucket[h];
      p->apBucket[h] = pN; }
    p->nKey++;
  }
  if( pN->nOp >= pN->nAlloc ){
    int newAlloc = pN->nAlloc ? pN->nAlloc*2 : 2;
    BfKeyOp *aNew = (BfKeyOp*)BFWAL_REALLOC(pN->aOp, newAlloc*(int)sizeof(BfKeyOp));
    if( aNew==0 ) return BFWAL_NOMEM;
    pN->aOp = aNew;
    pN->nAlloc = newAlloc;
  }
  pOp = &pN->aOp[pN->nOp];
  pOp->iFrame = iFrame;
  pOp->iSeq = p->iSeq;
  pOp->hint = pRec->pgno;
  pOp->op = pRec->op;
  pOp->nVal = pRec->op==BFWAL_OP_DELETE ? 0 : pRec->nVal;
  pOp->pVal = 0;
  if( pOp->nVal ){
    pOp->pVal = (u8*)BFWAL_MALLOC((int)pOp->nVal);
    if( pOp->pVal==0 ) return BFWAL_NOMEM;
    memcpy(pOp->pVal, pRec->pVal, pOp->nVal);
  }
  pN->nOp++;
  p->iSeq++;
  return BFWAL_OK;
}

/* Is op `o` of table root / key k covered by a marker that counts? */
static int bfIdxCovered(BfWalIndex *p, u32 root, const u8 *k, const BfKeyOp *o,
                        u32 mxFrame){
  int i;
  for(i=p->nMark-1; i>=0; i--){
    const BfMarker *m = &p->aMark[i];
    if( m->iSeq < o->iSeq ) break;              /* markers before the op */
    if( m->iFrame>mxFrame ) continue;           /* never honoured past the log end */
    if( m->root!=root ) continue;
    if( memcmp(k, m->lo, 8)<0 || memcmp(k, m->hi, 8)>0 ) continue;
    if( o->iFrame>m->snap && o->iFrame<m->start ) continue;  /* foreign, after snap */
    return 1;
  }
  return 0;
}

int sqlite3BfWalIndexKeyState(BfWalIndex *p, u32 root, const u8 *pKey,
    u32 mxFrame, int bKeepBeyond, BfKeyState *pOut){
  BfKeyNode *pN;
  int i;
  memset(pOut, 0, sizeof(*pOut));
  pOut->op = -1;
  if( p==0 ) return 0;
  pN = bfIdxFind(p, root, pKey);
  if( pN==0 ) return 0;
  for(i=pN->nOp-1; i>=0; i--){
    const BfKeyOp *o = &pN->aOp[i];
    if( o->iFrame>mxFrame && !bKeepBeyond ) continue;   /* torn tail */
    if( bfIdxCovered(p, root, pKey, o, mxFrame) ) return 1;  /* in base */
    pOut->op = o->op;
    pOut->pVal = o->pVal;
    pOut->nVal = o->nVal;
    pOut->iFrame = o->iFrame;
    pOut->hint = o->hint;
    return 1;
  }
  return 1;
}

int sqlite3BfWalIndexForEachKey(BfWalIndex *p,
    int (*xKey)(void *pCtx, u32 root, const u8 *pKey), void *pCtx){
  int i;
  if( p==0 || xKey==0 ) return BFWAL_OK;
  for(i=0; i<p->nBucket; i++){
    BfKeyNode *pN;
    for(pN = p->apBucket[i]; pN; pN = pN->pNext){
      int rc = xKey(pCtx, pN->root, pN->aKey);
      if( rc!=BFWAL_OK ) return rc;
    }
  }
  return BFWAL_OK;
}

int sqlite3BfWalIndexKeyCount(BfWalIndex *p){ return p ? p->nKey : 0; }

int sqlite3BfWalIndexAddFrame(BfWalIndex *p, const u8 *aBuf, int szBuf,
                              u32 iFrame){
  BfWalIter it;
  BfWalRec r;
  int rc;
  rc = sqlite3BfWalIterInit(&it, aBuf, szBuf);
  if( rc!=BFWAL_OK ) return rc;
  while( (rc = sqlite3BfWalIterNext(&it, &r))==BFWAL_OK ){
    int rc2 = sqlite3BfWalIndexAppend(p, &r, iFrame);
    if( rc2!=BFWAL_OK ) return rc2;
  }
  return (rc==BFWAL_DONE) ? BFWAL_OK : rc;
}

/* Ops and markers are appended in log order, so those past mxFrame are a
** suffix of each key's list and of the marker list. */
void sqlite3BfWalIndexPrune(BfWalIndex *p, u32 mxFrame){
  int i;
  if( p==0 ) return;
  for(i=0; i<p->nBucket; i++){
    BfKeyNode *pN;
    for(pN = p->apBucket[i]; pN; pN = pN->pNext){
      while( pN->nOp>0 && pN->aOp[pN->nOp-1].iFrame>mxFrame ){
        pN->nOp--;
        BFWAL_FREE(pN->aOp[pN->nOp].pVal);
        pN->aOp[pN->nOp].pVal = 0;
      }
    }
  }
  while( p->nMark>0 && p->aMark[p->nMark-1].iFrame>mxFrame ) p->nMark--;
}

#endif /* !defined(SQLITE_OMIT_BF_CACHE) */
