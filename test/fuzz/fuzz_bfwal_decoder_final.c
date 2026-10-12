/*
** Standalone fuzzer for BF-Tree WAL decoder - minimal SQLite integration
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int8_t i8;
typedef uint8_t u8;
typedef int16_t i16;
typedef uint16_t u16;
typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;

#define SQLITE_PRIVATE static

/* Minimal codec */
#define BFWAL_OK       0
#define BFWAL_FULL     1
#define BFWAL_DONE     2
#define BFWAL_CORRUPT  3
#define BFWAL_NOMEM    4

#define BFWAL_MAGIC     0x42465731u
#define BFWAL_VERSION   4
#define BFWAL_HDRSIZE   12
#define BFWAL_OP_INSERT 0
#define BFWAL_OP_DELETE 1
#define BFWAL_OP_CLEAR  2
#define BFWAL_VARINT_MAX 5

typedef struct {
  u32 pgno;
  u32 rootPgno;
  u8 op;
  u32 nKey;
  u32 nVal;
  const u8 *pKey;
  const u8 *pVal;
  u32 iFrame;
} BfWalRec;

typedef struct {
  const u8 *aBuf;
  int nUsed;
  int nRec;
  int off;
  int iRec;
} BfWalIter;

/* Inline codec implementation */
static u32 bfGet32(const u8 *p){
  return ((u32)p[0]<<24) | ((u32)p[1]<<16) | ((u32)p[2]<<8) | (u32)p[3];
}

static u32 bfGet16(const u8 *p){
  return ((u32)p[0]<<8) | (u32)p[1];
}

static int bfGetVarint(const u8 *p, int avail, u32 *pv){
  u32 v = 0;
  int i = 0, shift = 0;
  while( i<avail && i<BFWAL_VARINT_MAX ){
    u8 c = p[i++];
    if( shift==28 && (c&0xF0)!=0 ) return -1;
    v |= ((u32)(c & 0x7F)) << shift;
    if( (c & 0x80)==0 ){ *pv = v; return i; }
    shift += 7;
  }
  return -1;
}

static int sqlite3BfWalIterInit(BfWalIter *it, const u8 *aBuf, int szBuf){
  u32 nUsed, nRec;
  memset(it, 0, sizeof(*it));
  if( szBuf < BFWAL_HDRSIZE ) return BFWAL_CORRUPT;
  if( bfGet32(aBuf + 0) != BFWAL_MAGIC ) return BFWAL_CORRUPT;
  if( bfGet16(aBuf + 4) != BFWAL_VERSION ) return BFWAL_CORRUPT;
  nRec  = bfGet16(aBuf + 6);
  nUsed = bfGet32(aBuf + 8);
  if( nUsed < (u32)BFWAL_HDRSIZE || nUsed > (u32)szBuf ) return BFWAL_CORRUPT;
  it->aBuf = aBuf;
  it->nUsed = (int)nUsed;
  it->nRec = (int)nRec;
  it->off = BFWAL_HDRSIZE;
  it->iRec = 0;
  return BFWAL_OK;
}

static int sqlite3BfWalIterNext(BfWalIter *it, BfWalRec *pRec){
  const u8 *p;
  int off, avail, n;
  u32 pgno, rootPgno, nKey, nVal;
  u8 op;

  if( it->iRec >= it->nRec ) return BFWAL_DONE;

  off = it->off;
  avail = it->nUsed - off;
  if( avail < 9 ) return BFWAL_CORRUPT;
  p = it->aBuf + off;

  pgno = bfGet32(p);      p += 4;  off += 4;
  rootPgno = bfGet32(p);  p += 4;  off += 4;
  op = *p++;              off += 1;
  if( pgno==0 || rootPgno<=1 ) return BFWAL_CORRUPT;
  if( op!=BFWAL_OP_INSERT && op!=BFWAL_OP_DELETE && op!=BFWAL_OP_CLEAR ){
    return BFWAL_CORRUPT;
  }

  n = bfGetVarint(p, it->nUsed - off, &nKey);
  if( n<0 ) return BFWAL_CORRUPT;
  p += n; off += n;
  n = bfGetVarint(p, it->nUsed - off, &nVal);
  if( n<0 ) return BFWAL_CORRUPT;
  p += n; off += n;

  if( nKey > (u32)(it->nUsed - off) ) return BFWAL_CORRUPT;
  off += (int)nKey;
  if( nVal > (u32)(it->nUsed - off) ) return BFWAL_CORRUPT;
  if( op==BFWAL_OP_CLEAR && (nKey!=0 || nVal!=0) ) return BFWAL_CORRUPT;

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

/* ========== Main Fuzzer ========== */

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  if (Size < 12) return 0;

  BfWalIter it;
  BfWalRec rec;

  /* Test iterator robustness */
  int rc = sqlite3BfWalIterInit(&it, Data, (int)Size);
  if (rc == BFWAL_OK) {
    int count = 0;
    while (count < 10000) {  /* Bound iterations */
      rc = sqlite3BfWalIterNext(&it, &rec);
      if (rc != BFWAL_OK) break;
      count++;
    }
  }

  return 0;
}
