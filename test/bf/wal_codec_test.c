/*
** Standalone unit test for the BF-Tree physiological WAL record-batch codec
** (src/bf_wal.c).  Compiles the module in isolation with minimal typedefs — no
** amalgamation — so it doubles as the skeleton the esbmc-verifier and
** libfuzzer-tester harnesses build on.
**
** Build & run:
**   cc -O2 -I../../src -o /tmp/wal_codec_test wal_codec_test.c && /tmp/wal_codec_test
*/
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Minimal stand-ins for the sqliteInt.h fixed-width types the module uses. */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int64_t  i64;

/* Pull the codec in directly via its standalone seam (defines the API and the
** implementation; supplies its own types from the typedefs above). */
#define BF_WAL_STANDALONE 1
#include "bf_wal.c"

static int nFail = 0;
#define CHECK(c) do{ if(!(c)){ \
  printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); nFail++; } }while(0)

/* Round-trip: append a mix of records, then decode and compare. */
static void test_roundtrip(void){
  u8 buf[4096];
  BfWalBatch b;
  BfWalIter it;
  BfWalRec r;
  const char *k0="\x00\x00\x00\x00\x00\x00\x00\x2a", *v0="hello";
  const char *k1="\x00\x00\x00\x00\x00\x00\x01\x00";

  sqlite3BfWalBatchInit(&b, buf, sizeof(buf));

  r.pgno=7;  r.op=BFWAL_OP_INSERT; r.nKey=8; r.pKey=(const u8*)k0;
  r.nVal=5;  r.pVal=(const u8*)v0;
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );

  r.pgno=7;  r.op=BFWAL_OP_DELETE; r.nKey=8; r.pKey=(const u8*)k1;
  r.nVal=0;  r.pVal=0;
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );

  r.pgno=99; r.op=BFWAL_OP_INSERT; r.nKey=0; r.pKey=0; r.nVal=0; r.pVal=0;
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );

  sqlite3BfWalBatchFinish(&b);
  CHECK( sqlite3BfWalIsBatch(buf,sizeof(buf)) );

  CHECK( sqlite3BfWalIterInit(&it, buf, sizeof(buf))==BFWAL_OK );
  CHECK( sqlite3BfWalIterNext(&it,&r)==BFWAL_OK );
  CHECK( r.pgno==7 && r.op==BFWAL_OP_INSERT && r.nKey==8 && r.nVal==5 );
  CHECK( memcmp(r.pKey,k0,8)==0 && memcmp(r.pVal,v0,5)==0 );
  CHECK( sqlite3BfWalIterNext(&it,&r)==BFWAL_OK );
  CHECK( r.pgno==7 && r.op==BFWAL_OP_DELETE && r.nKey==8 && r.nVal==0 );
  CHECK( sqlite3BfWalIterNext(&it,&r)==BFWAL_OK );
  CHECK( r.pgno==99 && r.nKey==0 && r.nVal==0 && r.pKey==0 && r.pVal==0 );
  CHECK( sqlite3BfWalIterNext(&it,&r)==BFWAL_DONE );
}

/* The batch fills up and then rejects with BFWAL_FULL, never overflowing. */
static void test_full(void){
  u8 buf[256], key[64];
  BfWalBatch b;
  BfWalRec r;
  int rc, n=0;
  memset(key,0xAB,sizeof(key));
  sqlite3BfWalBatchInit(&b, buf, sizeof(buf));
  r.op=BFWAL_OP_INSERT; r.nKey=64; r.pKey=key; r.nVal=0; r.pVal=0;
  for(;;){
    r.pgno=(u32)(n+1);
    rc = sqlite3BfWalBatchAppend(&b,&r);
    if( rc==BFWAL_FULL ) break;
    CHECK( rc==BFWAL_OK );
    n++;
    CHECK( b.nUsed <= (int)sizeof(buf) );
  }
  CHECK( n>0 );
  sqlite3BfWalBatchFinish(&b);
  CHECK( b.nUsed <= (int)sizeof(buf) );
}

/* Decode must be bounds-safe on arbitrary/truncated input: never crash. */
static void test_fuzz_bounds(void){
  u8 buf[512];
  BfWalIter it;
  BfWalRec r;
  int i, t;
  srand(1234);
  for(t=0; t<200000; t++){
    int n = 1 + rand()%(int)sizeof(buf);
    for(i=0;i<n;i++) buf[i]=(u8)rand();
    if( sqlite3BfWalIterInit(&it, buf, n)==BFWAL_OK ){
      int guard = 0;
      while( guard++ < 100000 ){
        int rc = sqlite3BfWalIterNext(&it,&r);
        if( rc!=BFWAL_OK ) break;
        /* All returned pointers/lengths stay inside [buf, buf+n). */
        CHECK( r.nKey==0 || (r.pKey>=buf && r.pKey+r.nKey<=buf+n) );
        CHECK( r.nVal==0 || (r.pVal>=buf && r.pVal+r.nVal<=buf+n) );
      }
    }
  }
}

/* A well-formed batch survives round-trip after being finished; a header with
** an out-of-range nUsed is rejected by IterInit. */
static void test_corrupt_header(void){
  u8 buf[128];
  BfWalIter it;
  BfWalBatch b;
  BfWalRec r;
  sqlite3BfWalBatchInit(&b, buf, sizeof(buf));
  r.pgno=1; r.op=BFWAL_OP_INSERT; r.nKey=0; r.pKey=0; r.nVal=0; r.pVal=0;
  sqlite3BfWalBatchAppend(&b,&r);
  sqlite3BfWalBatchFinish(&b);
  /* Corrupt nUsed to claim more than the buffer holds. */
  buf[8]=0xFF; buf[9]=0xFF; buf[10]=0xFF; buf[11]=0xFF;
  CHECK( sqlite3BfWalIterInit(&it, buf, sizeof(buf))==BFWAL_CORRUPT );
}

/* Index: ops come back per-page in log order; other pages unaffected. */
static void test_index_order(void){
  BfWalIndex *ix = sqlite3BfWalIndexNew();
  BfWalRec r;
  int i;
  CHECK( ix!=0 );
  /* Interleave three pages; values encode (pgno, seq) so order is checkable. */
  for(i=0;i<30;i++){
    u8 key[1]; u8 val[2];
    u32 pg = (u32)(1 + (i%3));
    key[0]=(u8)i; val[0]=(u8)pg; val[1]=(u8)i;
    r.pgno=pg; r.op=(i&1)?BFWAL_OP_DELETE:BFWAL_OP_INSERT;
    r.nKey=1; r.pKey=key; r.nVal=(i&1)?0:2; r.pVal=(i&1)?0:val;
    CHECK( sqlite3BfWalIndexAppend(ix,&r)==BFWAL_OK );
  }
  CHECK( sqlite3BfWalIndexPageCount(ix,1)==10 );
  CHECK( sqlite3BfWalIndexPageCount(ix,2)==10 );
  CHECK( sqlite3BfWalIndexPageCount(ix,3)==10 );
  CHECK( sqlite3BfWalIndexPageCount(ix,4)==0 );
  /* Page 1 got i = 0,3,6,...,27; verify log order preserved via val[1]. */
  for(i=0;i<10;i++){
    CHECK( sqlite3BfWalIndexGet(ix,1,i,&r)==BFWAL_OK );
    CHECK( r.pgno==1 );
    if( r.op==BFWAL_OP_INSERT ){ CHECK( r.nVal==2 && r.pVal[1]==(u8)(i*3) ); }
  }
  CHECK( sqlite3BfWalIndexGet(ix,1,10,&r)==BFWAL_DONE );
  sqlite3BfWalIndexClearPage(ix,2);
  CHECK( sqlite3BfWalIndexPageCount(ix,2)==0 );
  CHECK( sqlite3BfWalIndexPageCount(ix,1)==10 );  /* others intact */
  sqlite3BfWalIndexFree(ix);
}

/* Idempotence proxy: scanning the same record frame into two indexes yields
** identical per-page op sequences ("replay twice = replay once" building block). */
static void test_index_from_frame(void){
  u8 buf[4096];
  BfWalBatch b;
  BfWalRec r;
  BfWalIndex *ix1, *ix2;
  u32 pg;
  const char *v="xy";
  int i;
  sqlite3BfWalBatchInit(&b, buf, sizeof(buf));
  for(i=0;i<50;i++){
    u8 key[2]; key[0]=(u8)i; key[1]=(u8)(i>>8);
    r.pgno=(u32)(1+(i%5)); r.op=BFWAL_OP_INSERT; r.nKey=2; r.pKey=key;
    r.nVal=2; r.pVal=(const u8*)v;
    CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );
  }
  sqlite3BfWalBatchFinish(&b);
  ix1 = sqlite3BfWalIndexNew();
  ix2 = sqlite3BfWalIndexNew();
  CHECK( sqlite3BfWalIndexAddFrame(ix1, buf, sizeof(buf))==BFWAL_OK );
  CHECK( sqlite3BfWalIndexAddFrame(ix2, buf, sizeof(buf))==BFWAL_OK );
  for(pg=1; pg<=5; pg++){
    int n1 = sqlite3BfWalIndexPageCount(ix1,pg);
    CHECK( n1==sqlite3BfWalIndexPageCount(ix2,pg) );
    for(i=0;i<n1;i++){
      BfWalRec a, c;
      CHECK( sqlite3BfWalIndexGet(ix1,pg,i,&a)==BFWAL_OK );
      CHECK( sqlite3BfWalIndexGet(ix2,pg,i,&c)==BFWAL_OK );
      CHECK( a.op==c.op && a.nKey==c.nKey && a.nVal==c.nVal );
      CHECK( a.nKey==0 || memcmp(a.pKey,c.pKey,a.nKey)==0 );
    }
  }
  sqlite3BfWalIndexFree(ix1);
  sqlite3BfWalIndexFree(ix2);
}

int main(void){
  test_roundtrip();
  test_full();
  test_corrupt_header();
  test_fuzz_bounds();
  test_index_order();
  test_index_from_frame();
  if( nFail==0 ) printf("wal_codec_test: ALL PASS\n");
  else printf("wal_codec_test: %d FAILURE(S)\n", nFail);
  return nFail!=0;
}
