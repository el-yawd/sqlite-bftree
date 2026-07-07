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

int main(void){
  test_roundtrip();
  test_full();
  test_corrupt_header();
  test_fuzz_bounds();
  if( nFail==0 ) printf("wal_codec_test: ALL PASS\n");
  else printf("wal_codec_test: %d FAILURE(S)\n", nFail);
  return nFail!=0;
}
