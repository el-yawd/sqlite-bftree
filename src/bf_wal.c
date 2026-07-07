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
  return 4                              /* pgno */
       + 1                              /* op   */
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
  if( pRec->nKey>0x3FFF ) return BFWAL_FULL;

  need = sqlite3BfWalRecSize(pRec);
  if( p->nUsed > p->szBuf - need ) return BFWAL_FULL;

  q = p->aBuf + p->nUsed;
  bfPut32(q, pRec->pgno);        q += 4;
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
  u32 pgno, nKey, nVal;
  u8 op;

  if( it->iRec >= it->nRec ) return BFWAL_DONE;

  off = it->off;
  avail = it->nUsed - off;
  if( avail < 5 ) return BFWAL_CORRUPT;      /* pgno(4) + op(1) */
  p = it->aBuf + off;

  pgno = bfGet32(p);  p += 4;  off += 4;
  op = *p++;          off += 1;
  if( pgno==0 ) return BFWAL_CORRUPT;
  if( op!=BFWAL_OP_INSERT && op!=BFWAL_OP_DELETE ) return BFWAL_CORRUPT;

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

  pRec->pgno = pgno;
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

/*
** Fast, side-effect-free classification of a frame payload as a record batch
** (magic + version + a self-consistent nUsed).  Lets the reader/recovery walk
** distinguish record frames from page-image frames.
*/
int sqlite3BfWalIsBatch(const u8 *aBuf, int szBuf){
  u32 nUsed;
  if( szBuf < BFWAL_HDRSIZE ) return 0;
  if( bfGet32(aBuf + 0) != BFWAL_MAGIC ) return 0;
  if( bfGet16(aBuf + 4) != BFWAL_VERSION ) return 0;
  nUsed = bfGet32(aBuf + 8);
  return nUsed >= (u32)BFWAL_HDRSIZE && nUsed <= (u32)szBuf;
}

#endif /* !defined(SQLITE_OMIT_BF_CACHE) */
