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

  r.pgno=7;  r.rootPgno=2; r.op=BFWAL_OP_INSERT; r.nKey=8; r.pKey=(const u8*)k0;
  r.nVal=5;  r.pVal=(const u8*)v0;
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );

  r.pgno=7;  r.rootPgno=2; r.op=BFWAL_OP_DELETE; r.nKey=8; r.pKey=(const u8*)k1;
  r.nVal=0;  r.pVal=0;
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );

  r.pgno=99; r.rootPgno=42; r.op=BFWAL_OP_INSERT; r.nKey=0; r.pKey=0; r.nVal=0; r.pVal=0;
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );

  sqlite3BfWalBatchFinish(&b);

  CHECK( sqlite3BfWalIterInit(&it, buf, sizeof(buf))==BFWAL_OK );
  CHECK( sqlite3BfWalIterNext(&it,&r)==BFWAL_OK );
  CHECK( r.pgno==7 && r.rootPgno==2 && r.op==BFWAL_OP_INSERT && r.nKey==8 && r.nVal==5 );
  CHECK( memcmp(r.pKey,k0,8)==0 && memcmp(r.pVal,v0,5)==0 );
  CHECK( sqlite3BfWalIterNext(&it,&r)==BFWAL_OK );
  CHECK( r.pgno==7 && r.rootPgno==2 && r.op==BFWAL_OP_DELETE && r.nKey==8 && r.nVal==0 );
  CHECK( sqlite3BfWalIterNext(&it,&r)==BFWAL_OK );
  CHECK( r.pgno==99 && r.rootPgno==42 && r.nKey==0 && r.nVal==0 && r.pKey==0 && r.pVal==0 );
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
  r.rootPgno=2; r.op=BFWAL_OP_INSERT; r.nKey=64; r.pKey=key; r.nVal=0; r.pVal=0;
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
  r.pgno=1; r.rootPgno=2; r.op=BFWAL_OP_INSERT; r.nKey=0; r.pKey=0; r.nVal=0; r.pVal=0;
  sqlite3BfWalBatchAppend(&b,&r);
  sqlite3BfWalBatchFinish(&b);
  /* Corrupt nUsed to claim more than the buffer holds. */
  buf[8]=0xFF; buf[9]=0xFF; buf[10]=0xFF; buf[11]=0xFF;
  CHECK( sqlite3BfWalIterInit(&it, buf, sizeof(buf))==BFWAL_CORRUPT );
}

/* Logical index (v5): a key's state is its LAST op; prune drops ops past the cut. */
static void mkKey(u8 *k, int n){ memset(k, 0, 8); k[0]=0x80; k[6]=(u8)(n>>8); k[7]=(u8)n; }
static void addOp(BfWalIndex *ix, u32 root, int key, int op, const char *v, u32 iFrame){
  BfWalRec r; u8 k[8];
  mkKey(k, key);
  r.pgno=9; r.rootPgno=root; r.op=(u8)op; r.nKey=8; r.pKey=k;
  r.nVal = v ? (u32)strlen(v) : 0; r.pVal=(const u8*)v;
  CHECK( sqlite3BfWalIndexAppend(ix,&r,iFrame)==BFWAL_OK );
}
static void addMerged(BfWalIndex *ix, u32 root, int lo, int hi, u32 snap, u32 start,
                      u32 iFrame){
  BfWalRec r; u8 k[16], v[8];
  mkKey(k, lo); mkKey(k+8, hi);
  v[0]=(u8)(snap>>24); v[1]=(u8)(snap>>16); v[2]=(u8)(snap>>8); v[3]=(u8)snap;
  v[4]=(u8)(start>>24); v[5]=(u8)(start>>16); v[6]=(u8)(start>>8); v[7]=(u8)start;
  r.pgno=9; r.rootPgno=root; r.op=BFWAL_OP_MERGED; r.nKey=16; r.pKey=k; r.nVal=8; r.pVal=v;
  CHECK( sqlite3BfWalIndexAppend(ix,&r,iFrame)==BFWAL_OK );
}
static int stateOf(BfWalIndex *ix, u32 root, int key, u32 mx, int keep, const char **pv){
  BfKeyState st; u8 k[8];
  mkKey(k, key);
  sqlite3BfWalIndexKeyState(ix, root, k, mx, keep, &st);
  if( pv ) *pv = st.op==BFWAL_OP_INSERT ? (const char*)st.pVal : 0;
  return st.op;
}
static void test_index_order(void){
  BfWalIndex *ix = sqlite3BfWalIndexNew();
  const char *v;
  CHECK( ix!=0 );
  addOp(ix, 2, 1, BFWAL_OP_INSERT, "a", 10);
  addOp(ix, 2, 1, BFWAL_OP_INSERT, "bb", 11);
  addOp(ix, 3, 1, BFWAL_OP_INSERT, "other-table", 11);
  addOp(ix, 2, 2, BFWAL_OP_INSERT, "x", 12);
  addOp(ix, 2, 2, BFWAL_OP_DELETE, 0, 13);
  CHECK( sqlite3BfWalIndexKeyCount(ix)==3 );
  CHECK( stateOf(ix,2,1,100,0,&v)==BFWAL_OP_INSERT && v && memcmp(v,"bb",2)==0 );
  CHECK( stateOf(ix,3,1,100,0,&v)==BFWAL_OP_INSERT && memcmp(v,"other-table",11)==0 );
  CHECK( stateOf(ix,2,2,100,0,0)==BFWAL_OP_DELETE );
  CHECK( stateOf(ix,2,3,100,0,0)==-1 );                  /* no op */
  /* Torn tail: ops past mxFrame are ignored unless bKeepBeyond. */
  CHECK( stateOf(ix,2,1,10,0,&v)==BFWAL_OP_INSERT && memcmp(v,"a",1)==0 );
  CHECK( stateOf(ix,2,1,10,1,&v)==BFWAL_OP_INSERT && memcmp(v,"bb",2)==0 );
  CHECK( stateOf(ix,2,2,12,0,0)==BFWAL_OP_INSERT );
  sqlite3BfWalIndexPrune(ix, 11);
  CHECK( stateOf(ix,2,2,100,0,0)==-1 );                  /* 12, 13 pruned */
  CHECK( stateOf(ix,2,1,100,0,&v)==BFWAL_OP_INSERT && memcmp(v,"bb",2)==0 );
  sqlite3BfWalIndexFree(ix);
}

/* MERGED markers (v5): cover earlier ops on keys in [lo,hi] of the table, not
** later ones, not other tables, not ops in frames (snap, start), and are never
** honoured past mxFrame. */
static void test_index_markers(void){
  BfWalIndex *ix = sqlite3BfWalIndexNew();
  addOp(ix, 2, 5, BFWAL_OP_INSERT, "old5", 10);
  addOp(ix, 2, 6, BFWAL_OP_INSERT, "old6", 10);
  addOp(ix, 2, 9, BFWAL_OP_INSERT, "out9", 10);           /* outside the range */
  addOp(ix, 3, 5, BFWAL_OP_INSERT, "t3", 10);             /* other table */
  addOp(ix, 2, 7, BFWAL_OP_INSERT, "foreign7", 12);       /* in (snap=11, start=14) */
  addMerged(ix, 2, 5, 8, 11, 14, 14);                     /* a merge, commit at 14 */
  addOp(ix, 2, 6, BFWAL_OP_INSERT, "new6", 14);           /* after the marker */
  CHECK( stateOf(ix,2,5,100,0,0)==-1 );                   /* covered: in base */
  CHECK( stateOf(ix,2,6,100,0,0)==BFWAL_OP_INSERT );      /* later op wins */
  CHECK( stateOf(ix,2,9,100,0,0)==BFWAL_OP_INSERT );
  CHECK( stateOf(ix,3,5,100,0,0)==BFWAL_OP_INSERT );
  CHECK( stateOf(ix,2,7,100,0,0)==BFWAL_OP_INSERT );      /* not in its image */
  CHECK( stateOf(ix,2,5,13,0,0)==BFWAL_OP_INSERT );       /* marker past the end */
  addOp(ix, 2, 8, BFWAL_OP_INSERT, "own8", 14);           /* own commit, after */
  addMerged(ix, 2, 8, 8, 11, 14, 15);                     /* same commit, later frame */
  CHECK( stateOf(ix,2,8,100,0,0)==-1 );                   /* own frame >= start */
  sqlite3BfWalIndexPrune(ix, 13);                         /* both markers go */
  CHECK( stateOf(ix,2,5,100,0,0)==BFWAL_OP_INSERT );
  sqlite3BfWalIndexFree(ix);
}

/* Idempotence proxy: scanning the same record frame into two indexes yields
** identical key states ("replay twice = replay once" building block). */
static void test_index_from_frame(void){
  u8 buf[4096];
  BfWalBatch b;
  BfWalRec r;
  BfWalIndex *ix1, *ix2;
  const char *v="xy";
  int i;
  sqlite3BfWalBatchInit(&b, buf, sizeof(buf));
  for(i=0;i<50;i++){
    u8 key[8]; mkKey(key, i%20);
    r.pgno=(u32)(1+(i%5)); r.rootPgno=77; r.op=(i%7)?BFWAL_OP_INSERT:BFWAL_OP_DELETE;
    r.nKey=8; r.pKey=key; r.nVal=r.op==BFWAL_OP_INSERT?2:0; r.pVal=(const u8*)v;
    CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );
  }
  sqlite3BfWalBatchFinish(&b);
  ix1 = sqlite3BfWalIndexNew();
  ix2 = sqlite3BfWalIndexNew();
  CHECK( sqlite3BfWalIndexAddFrame(ix1, buf, sizeof(buf), 1)==BFWAL_OK );
  CHECK( sqlite3BfWalIndexAddFrame(ix2, buf, sizeof(buf), 1)==BFWAL_OK );
  CHECK( sqlite3BfWalIndexKeyCount(ix1)==20 && sqlite3BfWalIndexKeyCount(ix2)==20 );
  for(i=0;i<20;i++){
    CHECK( stateOf(ix1,77,i,1,0,0)==stateOf(ix2,77,i,1,0,0) );
  }
  sqlite3BfWalIndexFree(ix1);
  sqlite3BfWalIndexFree(ix2);
}

/* BFWAL_OP_MERGED (payload v5): round-trips between ordinary ops; a marker
** whose key is not 16 bytes or whose value is not 8 is refused by the encoder
** and rejected by the decoder, and the retired CLEAR op (2) no longer decodes. */
static void test_merged_op(void){
  u8 buf[512];
  BfWalBatch b;
  BfWalIter it;
  BfWalRec r, o;
  u8 key[8] = {0x80,0,0,0,0,0,0,7};
  u8 range[16] = {0x80,0,0,0,0,0,0,1, 0x80,0,0,0,0,0,0,9};
  u8 sv[8] = {0,0,0,5, 0,0,0,7};
  int n = 0;
  sqlite3BfWalBatchInit(&b, buf, sizeof(buf));
  memset(&r, 0, sizeof(r));
  r.pgno=9; r.rootPgno=2; r.op=BFWAL_OP_INSERT; r.nKey=8; r.pKey=key;
  r.nVal=1; r.pVal=(const u8*)"v";
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );
  memset(&r, 0, sizeof(r));
  r.pgno=9; r.rootPgno=2; r.op=BFWAL_OP_MERGED; r.nKey=16; r.pKey=range;
  r.nVal=8; r.pVal=sv;
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );
  r.nKey=8;                                            /* short range */
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_FULL );
  r.nKey=16; r.nVal=0;                                 /* no snap/start */
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_FULL );
  r.op=2; r.nKey=0; r.nVal=0;                          /* the retired CLEAR */
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_FULL );
  sqlite3BfWalBatchFinish(&b);
  CHECK( sqlite3BfWalIterInit(&it, buf, sizeof(buf))==BFWAL_OK );
  while( sqlite3BfWalIterNext(&it, &o)==BFWAL_OK ){
    if( n==1 ){
      CHECK( o.op==BFWAL_OP_MERGED && o.nKey==16 && o.nVal==8 );
      CHECK( memcmp(o.pKey, range, 16)==0 && memcmp(o.pVal, sv, 8)==0 );
    }
    n++;
  }
  CHECK( n==2 );
  /* Hand-corrupt the marker's keyLen varint (after leaf,root,op) to 15. */
  {
    int off = BFWAL_HDRSIZE + 4+4+1+1+1+8+1;     /* skip the INSERT record */
    CHECK( buf[off+8]==BFWAL_OP_MERGED );
    buf[off+9] = 15;
    CHECK( sqlite3BfWalIterInit(&it, buf, sizeof(buf))==BFWAL_OK );
    CHECK( sqlite3BfWalIterNext(&it, &o)==BFWAL_OK );
    CHECK( sqlite3BfWalIterNext(&it, &o)==BFWAL_CORRUPT );
    buf[off+9] = 16;
    buf[off+8] = 2;                              /* the old CLEAR op */
    CHECK( sqlite3BfWalIterInit(&it, buf, sizeof(buf))==BFWAL_OK );
    CHECK( sqlite3BfWalIterNext(&it, &o)==BFWAL_OK );
    CHECK( sqlite3BfWalIterNext(&it, &o)==BFWAL_CORRUPT );
  }
}


/* C4b base ops (region entries): used only when a key has no log op; covered
** by any later marker over the key -- unless the base op keeps a frame inside
** the marker's (snap, start) exemption. */
static void setBase(BfWalIndex *ix, u32 root, int key, int op, const char *v, u32 f){
  u8 k[8]; mkKey(k, key);
  CHECK( sqlite3BfWalIndexSetBase(ix, root, k, (u8)op, (const u8*)v,
                                  v ? (u32)strlen(v) : 0, f)==BFWAL_OK );
}
static void test_base_ops(void){
  BfWalIndex *ix = sqlite3BfWalIndexNew();
  const char *v;
  u8 k[8];
  setBase(ix, 2, 1, BFWAL_OP_INSERT, "base1", 0);
  setBase(ix, 2, 2, BFWAL_OP_DELETE, 0, 0);
  setBase(ix, 2, 3, BFWAL_OP_INSERT, "base3", 12);       /* frame kept (same gen) */
  setBase(ix, 2, 4, BFWAL_OP_INSERT, "base4", 0);
  mkKey(k, 1); CHECK( sqlite3BfWalIndexHasBase(ix, 2, k) );
  mkKey(k, 9); CHECK( !sqlite3BfWalIndexHasBase(ix, 2, k) );
  CHECK( stateOf(ix,2,1,100,0,&v)==BFWAL_OP_INSERT && memcmp(v,"base1",5)==0 );
  CHECK( stateOf(ix,2,2,100,0,0)==BFWAL_OP_DELETE );
  addOp(ix, 2, 4, BFWAL_OP_INSERT, "log4", 20);           /* a log op wins */
  CHECK( stateOf(ix,2,4,100,0,&v)==BFWAL_OP_INSERT && memcmp(v,"log4",4)==0 );
  CHECK( stateOf(ix,2,4,19,0,&v)==BFWAL_OP_INSERT && memcmp(v,"base4",5)==0 );
  addMerged(ix, 2, 1, 3, 11, 14, 25);                     /* covers 1..3 ... */
  CHECK( stateOf(ix,2,1,100,0,0)==-1 );
  CHECK( stateOf(ix,2,2,100,0,0)==-1 );
  CHECK( stateOf(ix,2,3,100,0,0)==BFWAL_OP_INSERT );      /* ... but 3 is exempt */
  CHECK( stateOf(ix,2,1,24,0,0)==BFWAL_OP_INSERT );       /* marker past the end */
  setBase(ix, 2, 1, BFWAL_OP_INSERT, "again", 0);          /* SetBase replaces */
  CHECK( stateOf(ix,2,1,24,0,&v)==BFWAL_OP_INSERT && memcmp(v,"again",5)==0 );
  sqlite3BfWalIndexFree(ix);
}

/* C4b rebase: folding the log into base ops preserves every key's state;
** markers and log ops are gone; frames are kept only when asked (a CKPT in the
** same generation), so a later marker's exemption still sees them. */
static void test_rebase(void){
  BfWalIndex *ix = sqlite3BfWalIndexNew();
  int i, st[12];
  const char *v;
  setBase(ix, 2, 1, BFWAL_OP_INSERT, "b1", 0);
  setBase(ix, 2, 2, BFWAL_OP_INSERT, "b2", 0);
  addOp(ix, 2, 2, BFWAL_OP_INSERT, "l2", 5);
  addOp(ix, 2, 3, BFWAL_OP_INSERT, "l3", 6);
  addOp(ix, 2, 4, BFWAL_OP_DELETE, 0, 7);
  addOp(ix, 2, 5, BFWAL_OP_INSERT, "l5", 8);
  addMerged(ix, 2, 5, 6, 8, 9, 9);                        /* covers 5 (frame 8 <= snap) */
  addOp(ix, 2, 6, BFWAL_OP_INSERT, "l6", 9);              /* own op, after */
  addOp(ix, 2, 7, BFWAL_OP_INSERT, "l7", 8);              /* exempt from it */
  addMerged(ix, 2, 7, 7, 7, 9, 10);
  for(i=1;i<=8;i++) st[i] = stateOf(ix,2,i,10,0,0);
  sqlite3BfWalIndexRebase(ix, 10, 1);
  for(i=1;i<=8;i++) CHECK( stateOf(ix,2,i,100,0,0)==st[i] );
  CHECK( stateOf(ix,2,2,100,0,&v)==BFWAL_OP_INSERT && memcmp(v,"l2",2)==0 );
  CHECK( stateOf(ix,2,5,100,0,0)==-1 );                   /* covered: dropped */
  CHECK( sqlite3BfWalIndexKeyCount(ix)==6 );              /* 1,2,3,4,6,7 */
  /* A later BEGIN CONCURRENT merge with snap=7 exempts frame 8 (key 7)... */
  addMerged(ix, 2, 1, 9, 7, 12, 12);
  CHECK( stateOf(ix,2,7,100,0,0)==BFWAL_OP_INSERT );
  CHECK( stateOf(ix,2,3,100,0,0)==-1 );                   /* frame 6 <= snap */
  CHECK( stateOf(ix,2,6,100,0,0)==BFWAL_OP_INSERT );      /* frame 9: exempt too */
  /* ... and a restart fold (frames dropped) then makes every base op coverable. */
  sqlite3BfWalIndexRebase(ix, 100, 0);
  CHECK( sqlite3BfWalIndexKeyCount(ix)==2 );              /* 6 and 7 survive */
  addMerged(ix, 2, 1, 9, 0, 1, 1);
  CHECK( stateOf(ix,2,6,100,0,0)==-1 );
  CHECK( stateOf(ix,2,7,100,0,0)==-1 );
  sqlite3BfWalIndexFree(ix);
}

/* CKPT op (payload v6): round-trips, carries no key or value, and is noted by
** the index without becoming an op of any key. */
static void test_ckpt_op(void){
  u8 buf[512];
  BfWalBatch b;
  BfWalIter it;
  BfWalRec r, o;
  BfWalIndex *ix = sqlite3BfWalIndexNew();
  u8 k[8];
  mkKey(k, 3);
  sqlite3BfWalBatchInit(&b, buf, sizeof(buf));
  memset(&r, 0, sizeof(r));
  r.pgno=9; r.rootPgno=2; r.op=BFWAL_OP_INSERT; r.nKey=8; r.pKey=k; r.nVal=1; r.pVal=(const u8*)"x";
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );
  memset(&r, 0, sizeof(r));
  r.pgno=1; r.rootPgno=2; r.op=BFWAL_OP_CKPT;
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_FULL );    /* v7: needs its frame */
  r.nVal=4; r.pVal=(const u8*)"\x00\x00\x01\x2c";      /* segment frame 300 */
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_OK );
  r.nKey=8; r.pKey=k;
  CHECK( sqlite3BfWalBatchAppend(&b,&r)==BFWAL_FULL );    /* CKPT with a key */
  sqlite3BfWalBatchFinish(&b);
  CHECK( sqlite3BfWalIterInit(&it, buf, sizeof(buf))==BFWAL_OK );
  CHECK( sqlite3BfWalIterNext(&it,&o)==BFWAL_OK && o.op==BFWAL_OP_INSERT );
  CHECK( sqlite3BfWalIterNext(&it,&o)==BFWAL_OK && o.op==BFWAL_OP_CKPT && o.nKey==0
      && o.nVal==4 );
  CHECK( sqlite3BfWalIterNext(&it,&o)==BFWAL_DONE );
  CHECK( sqlite3BfWalIndexAddFrame(ix, buf, sizeof(buf), 7)==BFWAL_OK );
  CHECK( sqlite3BfWalIndexKeyCount(ix)==1 );
  { u32 iSeg = 0;
    CHECK( sqlite3BfWalIndexTakeCkpt(ix, &iSeg)==7 && iSeg==300 );
    CHECK( sqlite3BfWalIndexTakeCkpt(ix, &iSeg)==0 ); }
  /* A CKPT that claims a key does not decode. */
  buf[BFWAL_HDRSIZE + (4+4+1+1+1+8+1) + 9] = 8;           /* its keyLen varint */
  CHECK( sqlite3BfWalIterInit(&it, buf, sizeof(buf))==BFWAL_OK );
  CHECK( sqlite3BfWalIterNext(&it,&o)==BFWAL_OK );
  CHECK( sqlite3BfWalIterNext(&it,&o)!=BFWAL_OK );
  sqlite3BfWalIndexFree(ix);
}

/* C4b region entry codec: round trip of INSERT / DELETE / KILL, and every
** truncation of an entry is rejected (0) rather than read past. */
static void test_region_codec(void){
  u8 a[256], lo[8], hi[8];
  BfRegionEntry e;
  int n, n1, n2, n3, i;
  mkKey(lo, 5); mkKey(hi, 9);
  n1 = sqlite3BfRegionEncode(a, BFWAL_OP_INSERT, 7, lo, (const u8*)"value", 5, 123);
  CHECK( n1==sqlite3BfRegionEntrySize(BFWAL_OP_INSERT, 5) );
  n2 = sqlite3BfRegionEncode(a+n1, BFWAL_OP_DELETE, 7, hi, 0, 0, 124);
  CHECK( n2==sqlite3BfRegionEntrySize(BFWAL_OP_DELETE, 0) );
  n3 = sqlite3BfRegionEncode(a+n1+n2, BFREG_KILL, 8, lo, hi, 8, 0);
  CHECK( n3==sqlite3BfRegionEntrySize(BFREG_KILL, 8) );
  n = sqlite3BfRegionDecode(a, n1+n2+n3, &e);
  CHECK( n==n1 && e.kind==BFWAL_OP_INSERT && e.root==7 && e.iFrame==123
      && e.nVal==5 && memcmp(e.pVal,"value",5)==0 && memcmp(e.pKey,lo,8)==0 );
  n = sqlite3BfRegionDecode(a+n1, n2+n3, &e);
  CHECK( n==n2 && e.kind==BFWAL_OP_DELETE && e.iFrame==124 && memcmp(e.pKey,hi,8)==0 );
  n = sqlite3BfRegionDecode(a+n1+n2, n3, &e);
  CHECK( n==n3 && e.kind==BFREG_KILL && e.root==8 && memcmp(e.pHi,hi,8)==0 );
  for(i=0;i<n1;i++) CHECK( sqlite3BfRegionDecode(a, i, &e)==0 );
  for(i=0;i<n3;i++) CHECK( sqlite3BfRegionDecode(a+n1+n2, i, &e)==0 );
  { u8 pz[8]; memset(pz, 0, 8);
    n = sqlite3BfRegionEncode(a+200, BFREG_PAGE, 4321, pz, 0, 0, 0);
    CHECK( n==sqlite3BfRegionEntrySize(BFREG_PAGE, 0) );
    CHECK( sqlite3BfRegionDecode(a+200, n, &e)==n && e.kind==BFREG_PAGE && e.root==4321 ); }
  a[0] = 9;  CHECK( sqlite3BfRegionDecode(a, n1, &e)==0 );            /* bad kind */
  a[0] = BFWAL_OP_INSERT; a[1]=a[2]=a[3]=0; a[4]=1;
  CHECK( sqlite3BfRegionDecode(a, n1, &e)==0 );                        /* root 1 */
}

int main(void){
  test_roundtrip();
  test_full();
  test_corrupt_header();
  test_fuzz_bounds();
  test_index_order();
  test_index_markers();
  test_index_from_frame();
  test_merged_op();
  test_base_ops();
  test_rebase();
  test_ckpt_op();
  test_region_codec();
  if( nFail==0 ) printf("wal_codec_test: ALL PASS\n");
  else printf("wal_codec_test: %d FAILURE(S)\n", nFail);
  return nFail!=0;
}
