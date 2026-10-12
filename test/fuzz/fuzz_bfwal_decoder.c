/*
** Fuzzer for BF-Tree WAL record-batch decoder and index.
**
** Tests:
** 1. Decoder robustness: sqlite3BfWalIterInit/Next with arbitrary payloads
** 2. Index operations: sqlite3BfWalIndexAddFrame and queries
** 3. Prune operation: drop frames past a given marker
**
** All operations must be bounds-safe; BFWAL_CORRUPT is an acceptable error.
** Must never crash, read OOB, or trigger ASAN/UBSan.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations from sqlite3.h (avoid including full header) */
#ifdef __cplusplus
extern "C" {
#endif

typedef int8_t i8;
typedef uint8_t u8;
typedef int16_t i16;
typedef uint16_t u16;
typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;

/* BF-Tree WAL types and functions */
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

typedef struct BfWalRec BfWalRec;
struct BfWalRec {
  u32       pgno;
  u32       rootPgno;
  u8        op;
  u32       nKey;
  u32       nVal;
  const u8 *pKey;
  const u8 *pVal;
  u32       iFrame;
};

typedef struct BfWalIter BfWalIter;
struct BfWalIter {
  const u8 *aBuf;
  int nUsed;
  int nRec;
  int off;
  int iRec;
};

typedef struct BfWalIndex BfWalIndex;

/* Forward declarations of functions from amalgamation */
int  sqlite3BfWalIterInit(BfWalIter *it, const u8 *aBuf, int szBuf);
int  sqlite3BfWalIterNext(BfWalIter *it, BfWalRec *pRec);
BfWalIndex *sqlite3BfWalIndexNew(void);
void        sqlite3BfWalIndexFree(BfWalIndex *p);
int         sqlite3BfWalIndexAppend(BfWalIndex *p, const BfWalRec *pRec, u32 iFrame);
int         sqlite3BfWalIndexPageCount(BfWalIndex *p, u32 pgno);
int         sqlite3BfWalIndexGet(BfWalIndex *p, u32 pgno, int i, BfWalRec *pRec);
int         sqlite3BfWalIndexAddFrame(BfWalIndex *p, const u8 *aBuf, int szBuf, u32 iFrame);
int         sqlite3BfWalIndexForEachPage(BfWalIndex *p, int (*xPage)(void *pCtx, u32 pgno), void *pCtx);
void        sqlite3BfWalIndexPrune(BfWalIndex *p, u32 mxFrame);

#ifdef __cplusplus
}
#endif

/* FuzzedDataProvider-like helper to split the input bytes */
typedef struct {
  const uint8_t *data;
  size_t pos;
  size_t size;
} FuzzInput;

static uint32_t fuzz_read_u32(FuzzInput *p) {
  if (p->pos + 4 > p->size) return 0;
  uint32_t v = ((uint32_t)p->data[p->pos] << 24)
             | ((uint32_t)p->data[p->pos+1] << 16)
             | ((uint32_t)p->data[p->pos+2] << 8)
             | ((uint32_t)p->data[p->pos+3]);
  p->pos += 4;
  return v;
}

static uint8_t fuzz_read_u8(FuzzInput *p) {
  if (p->pos >= p->size) return 0;
  return p->data[p->pos++];
}

static size_t fuzz_read_size(FuzzInput *p, size_t max_val) {
  if (p->pos >= p->size) return 0;
  size_t sz = p->data[p->pos++];
  return (sz % (max_val + 1));
}

/* Callback for ForEachPage: just count pages */
static int fuzz_page_callback(void *pCtx, u32 pgno) {
  int *count = (int*)pCtx;
  (*count)++;
  if (*count > 1000) return BFWAL_CORRUPT; /* Stop excessive iteration */
  (void)pgno; /* Suppress unused warning */
  return BFWAL_OK;
}

extern int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  FuzzInput input = {Data, 0, Size};

  if (Size < 4) return 0; /* Nothing useful to fuzz */

  /* Test 1: Decoder robustness with various payload sizes */
  uint32_t szPage = fuzz_read_u32(&input);
  if (szPage == 0) szPage = 4096;
  if (szPage > 65536) szPage = 65536;

  size_t payload_len = fuzz_read_size(&input, Size);
  if (payload_len > Size - input.pos) payload_len = Size - input.pos;
  if (payload_len == 0) return 0;

  const uint8_t *payload = Data + input.pos;
  input.pos += payload_len;

  /* Test the iterator directly */
  BfWalIter it;
  BfWalRec rec;
  int rc = sqlite3BfWalIterInit(&it, payload, (int)payload_len);
  if (rc == BFWAL_OK) {
    /* Iterate through records */
    int record_count = 0;
    while (record_count < 1000) { /* Bound iteration to prevent infinite loops */
      rc = sqlite3BfWalIterNext(&it, &rec);
      if (rc == BFWAL_DONE) break;
      if (rc != BFWAL_OK) break;
      record_count++;
    }
  }

  /* Test 2: Index operations */
  BfWalIndex *pIdx = sqlite3BfWalIndexNew();
  if (pIdx == NULL) return 0;

  /* Add the frame to the index */
  uint32_t iFrame = fuzz_read_u32(&input);
  int rc2 = sqlite3BfWalIndexAddFrame(pIdx, payload, (int)payload_len, iFrame);

  if (rc2 == BFWAL_OK || rc2 == BFWAL_CORRUPT) {
    /* Query the index */
    uint32_t query_pgno = fuzz_read_u32(&input);
    if (query_pgno == 0) query_pgno = 1;

    int count = sqlite3BfWalIndexPageCount(pIdx, query_pgno);
    if (count > 0) {
      /* Try to get records from this page */
      for (int i = 0; i < count && i < 100; i++) {
        BfWalRec q_rec;
        int qrc = sqlite3BfWalIndexGet(pIdx, query_pgno, i, &q_rec);
        (void)qrc; /* Use the result to prevent optimization */
      }
    }

    /* Test ForEachPage callback */
    int page_visit_count = 0;
    (void)sqlite3BfWalIndexForEachPage(pIdx, fuzz_page_callback, &page_visit_count);

    /* Test prune with random mxFrame */
    uint32_t mxFrame = fuzz_read_u32(&input);
    sqlite3BfWalIndexPrune(pIdx, mxFrame);
  }

  sqlite3BfWalIndexFree(pIdx);

  return 0;
}
