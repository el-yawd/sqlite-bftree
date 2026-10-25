/*
** ESBMC harness: rowid encoding preserves order and round-trips.
**
** Properties:
**  1. dec(enc(a)) == a for all i64 a
**  2. a < b <=> memcmp(enc(a), enc(b), 8) < 0
**  3. bfKeyCompare agrees with memcmp for 8-byte keys
**
** Encoding: i64 rowid ^ (1LL << 63), encoded big-endian as 8 bytes.
** This flips the sign bit so that byte order matches numeric order.
*/

#include "bf_shim.h"
#include <string.h>
#include <stdint.h>

/* Extracted from src/bf_btree.c */
static void bfEncodeRowid(i64 rowid, u8 *pBuf){
  u64 u = (u64)rowid ^ ((u64)1<<63);
  int i;
  for(i=7; i>=0; i--){ pBuf[i]=(u8)(u&0xff); u>>=8; }
}

static i64 bfDecodeRowid(const u8 *pKey){
  u64 u = 0;
  int i;
  for(i=0; i<8; i++) u = (u<<8) | pKey[i];
  return (i64)(u ^ ((u64)1<<63));
}

/* Extracted from src/bf_mini_page.c */
static int bfKeyCompare(const u8 *pKey1, int nKey1, const u8 *pKey2, int nKey2){
  int n, rc;
  if( nKey1==8 && nKey2==8 ){
    u64 a = ((u64)pKey1[0]<<56)|((u64)pKey1[1]<<48)|((u64)pKey1[2]<<40)
          | ((u64)pKey1[3]<<32)|((u64)pKey1[4]<<24)|((u64)pKey1[5]<<16)
          | ((u64)pKey1[6]<<8)|(u64)pKey1[7];
    u64 b = ((u64)pKey2[0]<<56)|((u64)pKey2[1]<<48)|((u64)pKey2[2]<<40)
          | ((u64)pKey2[3]<<32)|((u64)pKey2[4]<<24)|((u64)pKey2[5]<<16)
          | ((u64)pKey2[6]<<8)|(u64)pKey2[7];
    return a<b ? -1 : (a>b);
  }
  n = nKey1 < nKey2 ? nKey1 : nKey2;
  rc = memcmp(pKey1, pKey2, n);
  if( rc == 0 ){
    rc = nKey1 - nKey2;
  }
  return rc;
}

int main(void){
  i64 a, b;
  u8 enc_a[8], enc_b[8];
  i64 dec_a, dec_b;
  int cmp_memcmp, cmp_bfKeyCompare;

  /* Symbolic input: two arbitrary i64 values */
  a = __VERIFIER_nondet_long();
  b = __VERIFIER_nondet_long();

  /* Encode both */
  bfEncodeRowid(a, enc_a);
  bfEncodeRowid(b, enc_b);

  /* Property 1: dec(enc(a)) == a */
  dec_a = bfDecodeRowid(enc_a);
  __ESBMC_assert(dec_a == a, "Property 1a: dec(enc(a)) == a");

  /* Property 1b: dec(enc(b)) == b */
  dec_b = bfDecodeRowid(enc_b);
  __ESBMC_assert(dec_b == b, "Property 1b: dec(enc(b)) == b");

  /* Property 2a: a == b => memcmp(...) == 0 */
  cmp_memcmp = memcmp(enc_a, enc_b, 8);
  if( a == b ){
    __ESBMC_assert(cmp_memcmp == 0, "Property 2a: a==b => memcmp==0");
  }

  /* Property 2b: a < b => memcmp(...) < 0 */
  if( a < b ){
    __ESBMC_assert(cmp_memcmp < 0, "Property 2b: a<b => memcmp<0");
  }

  /* Property 2c: a > b => memcmp(...) > 0 */
  if( a > b ){
    __ESBMC_assert(cmp_memcmp > 0, "Property 2c: a>b => memcmp>0");
  }

  /* Property 3: bfKeyCompare agrees with memcmp for 8-byte keys */
  cmp_bfKeyCompare = bfKeyCompare(enc_a, 8, enc_b, 8);
  /* The sign of memcmp and bfKeyCompare must match */
  if( cmp_memcmp < 0 ){
    __ESBMC_assert(cmp_bfKeyCompare < 0, "Property 3a: bfKeyCompare agrees (negative)");
  }
  if( cmp_memcmp == 0 ){
    __ESBMC_assert(cmp_bfKeyCompare == 0, "Property 3b: bfKeyCompare agrees (zero)");
  }
  if( cmp_memcmp > 0 ){
    __ESBMC_assert(cmp_bfKeyCompare > 0, "Property 3c: bfKeyCompare agrees (positive)");
  }

  return 0;
}
