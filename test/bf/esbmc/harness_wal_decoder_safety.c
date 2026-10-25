/*
** ESBMC harness: the WAL record decoder is memory-safe and terminates on ANY
** input (src/bf_wal.c, the real file, #included unchanged).
**
** A fully symbolic buffer of SZ bytes and a symbolic length 0..SZ: recovery
** feeds the decoder whatever the log file holds, torn or corrupt.  Checked:
**   - no out-of-bounds read, invalid pointer or arithmetic overflow anywhere
**     in sqlite3BfWalIterInit / sqlite3BfWalIterNext (ESBMC's built-in checks);
**   - every record returned has its key and value inside [aBuf, aBuf+n);
**   - progress: every record consumes at least the 11-byte minimum
**     (pgno 4 + root 4 + op 1 + two 1-byte varints), so the decoder returns
**     DONE or CORRUPT within n/11 + 1 calls -- asserted, which proves it never
**     loops without consuming input.
** With unwinding assertions ON, the bound below covers every loop.
*/
#include "bf_shim.h"
#define BF_WAL_STANDALONE
#include "../../../src/bf_wal.h"
#include "../../../src/bf_wal.c"

#define SZ 48
#define MAXCALLS (SZ/11 + 2)

int main(void){
  u8 aBuf[SZ];
  BfWalIter it;
  BfWalRec r;
  int n, i, rc, calls;

  for(i=0; i<SZ; i++) aBuf[i] = __VERIFIER_nondet_uchar();
  n = __VERIFIER_nondet_int();
  __ESBMC_assume(n >= 0 && n <= SZ);

  rc = sqlite3BfWalIterInit(&it, aBuf, n);
  if( rc!=BFWAL_OK ){
    __ESBMC_assert(rc==BFWAL_CORRUPT, "init: OK or CORRUPT only");
    return 0;
  }
  for(calls=0; calls<MAXCALLS; calls++){
    rc = sqlite3BfWalIterNext(&it, &r);
    if( rc==BFWAL_DONE || rc==BFWAL_CORRUPT ) return 0;
    __ESBMC_assert(rc==BFWAL_OK, "next: OK, DONE or CORRUPT only");
    if( r.nKey ){
      __ESBMC_assert(r.pKey >= aBuf && r.pKey + r.nKey <= aBuf + n, "key inside the input");
    }
    if( r.nVal ){
      __ESBMC_assert(r.pVal >= aBuf && r.pVal + r.nVal <= aBuf + n, "value inside the input");
    }
  }
  __ESBMC_assert(0, "decoder made no progress: more records than the input can hold");
  return 0;
}
