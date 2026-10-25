/*
** ESBMC harness: the WAL record-batch codec round-trips (src/bf_wal.c, the
** real file, #included unchanged).
**
** For 0..2 records, each with a SYMBOLIC op among INSERT / DELETE / MERGED /
** CKPT, symbolic leaf and root page numbers and symbolic key/value bytes
** (INSERT: 8-byte key, value length 0..8; DELETE: 8-byte key, no value;
** MERGED: 16-byte key range, 8-byte value; CKPT: no key, 4-byte value):
** whatever sqlite3BfWalBatchAppend accepts, sqlite3BfWalIterNext returns in
** order with identical fields and bytes, then BFWAL_DONE.  Records the
** encoder refuses (BFWAL_FULL: malformed or too big) are skipped, as a
** caller would.
**
** Buffer: 64 bytes, so every loop -- including BatchFinish's zero fill and
** the decoder's -- is fully unwound and checked (--unwind 70 with unwinding
** assertions ON; see README.md).  Configurations, to keep each run
** tractable (2 records x 4 symbolic ops did not finish in 30 min):
**   -DOPFIX=k      1 record, op fixed to k (0 INSERT, 1 DELETE, 3 MERGED,
**                  4 CKPT) -- one run per kind
**   -DROWOPS2      2 records, op symbolic among INSERT/DELETE (record order)
** A 64-byte batch also makes some appends return BFWAL_FULL (that path is
** checked too: a refused record never reaches the decoder).
*/
#include "bf_shim.h"
#define BF_WAL_STANDALONE
#include "../../../src/bf_wal.h"
#include "../../../src/bf_wal.c"

#define SZ 64
#ifdef ROWOPS2
# define NMAX 2
#else
# define NMAX 1
#endif

int main(void){
  u8 aBuf[SZ];
  BfWalBatch batch;
  BfWalIter it;
  BfWalRec rec[NMAX], got;
  u8 key[NMAX][16], val[NMAX][8];
  int nTry, nOk = 0, i, j, rc;
  int okIdx[NMAX];

  nTry = __VERIFIER_nondet_int();
  __ESBMC_assume(nTry >= 0 && nTry <= NMAX);
  sqlite3BfWalBatchInit(&batch, aBuf, SZ);

  for(i=0; i<nTry; i++){
    int op = __VERIFIER_nondet_int();
#if defined(ROWOPS2)
    __ESBMC_assume(op==BFWAL_OP_INSERT || op==BFWAL_OP_DELETE);
#elif defined(OPFIX)
    op = OPFIX;
#else
    __ESBMC_assume(op==BFWAL_OP_INSERT || op==BFWAL_OP_DELETE
                || op==BFWAL_OP_MERGED || op==BFWAL_OP_CKPT);
#endif
    for(j=0; j<16; j++) key[i][j] = __VERIFIER_nondet_uchar();
    for(j=0; j<8; j++)  val[i][j] = __VERIFIER_nondet_uchar();
    rec[i].pgno = __VERIFIER_nondet_uint();
    rec[i].rootPgno = __VERIFIER_nondet_uint();
    rec[i].op = (u8)op;
    rec[i].iFrame = 0;
    if( op==BFWAL_OP_INSERT ){
      unsigned v = __VERIFIER_nondet_uint();
      __ESBMC_assume(v <= 8);
      rec[i].nKey = 8; rec[i].nVal = v;
    }else if( op==BFWAL_OP_DELETE ){
      rec[i].nKey = 8; rec[i].nVal = 0;
    }else if( op==BFWAL_OP_MERGED ){
      rec[i].nKey = 16; rec[i].nVal = 8;
    }else{
      rec[i].nKey = 0; rec[i].nVal = 4;
    }
    rec[i].pKey = rec[i].nKey ? key[i] : 0;
    rec[i].pVal = rec[i].nVal ? val[i] : 0;
    rc = sqlite3BfWalBatchAppend(&batch, &rec[i]);
    __ESBMC_assert(rc==BFWAL_OK || rc==BFWAL_FULL, "append: OK or FULL only");
    if( rc==BFWAL_OK ) okIdx[nOk++] = i;
  }
  sqlite3BfWalBatchFinish(&batch);

  rc = sqlite3BfWalIterInit(&it, aBuf, SZ);
  __ESBMC_assert(rc==BFWAL_OK, "a finished batch always opens");
  for(i=0; i<nOk; i++){
    const BfWalRec *r = &rec[okIdx[i]];
    rc = sqlite3BfWalIterNext(&it, &got);
    __ESBMC_assert(rc==BFWAL_OK, "every appended record decodes");
    __ESBMC_assert(got.pgno==r->pgno, "pgno round-trips");
    __ESBMC_assert(got.rootPgno==r->rootPgno, "rootPgno round-trips");
    __ESBMC_assert(got.op==r->op, "op round-trips");
    __ESBMC_assert(got.nKey==r->nKey, "nKey round-trips");
    __ESBMC_assert(got.nVal==r->nVal, "nVal round-trips");
    for(j=0; j<(int)r->nKey; j++){
      __ESBMC_assert(got.pKey[j]==r->pKey[j], "key bytes round-trip");
    }
    for(j=0; j<(int)r->nVal; j++){
      __ESBMC_assert(got.pVal[j]==r->pVal[j], "value bytes round-trip");
    }
  }
  rc = sqlite3BfWalIterNext(&it, &got);
  __ESBMC_assert(rc==BFWAL_DONE, "nothing after the last record");
  return 0;
}
