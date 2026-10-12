/*
** Minimal, self-contained fuzzer for BF-Tree WAL record-batch decoder.
** Includes the codec directly to avoid linking issues.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Types */
typedef int8_t i8;
typedef uint8_t u8;
typedef int16_t i16;
typedef uint16_t u16;
typedef int32_t i32;
typedef uint32_t u32;
typedef int64_t i64;
typedef uint64_t u64;

/* Minimal SQLite compatibility */
#define SQLITE_PRIVATE static
#define SQLITE_OMIT_BF_CACHE 0

/* Include codec header first */
#include "../../src/bf_wal.h"

/* Memory functions */
static void *alloc_malloc(size_t n) { return malloc(n); }
static void *alloc_realloc(void *p, size_t n) { return realloc(p, n); }
static void alloc_free(void *p) { free(p); }

#define BF_WAL_STANDALONE 1
#include "../../src/bf_wal.c"

/* ========== Fuzzer ========== */

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  if (Size < 16) return 0;

  /* Test 1: Iterator robustness with various payload sizes */
  BfWalIter it;
  BfWalRec rec;

  int rc = sqlite3BfWalIterInit(&it, Data, (int)Size);
  if (rc == BFWAL_OK) {
    int record_count = 0;
    while (record_count < 1000) {
      rc = sqlite3BfWalIterNext(&it, &rec);
      if (rc != BFWAL_OK) break;
      record_count++;
    }
  }

  /* Test 2: Index building from various payloads */
  BfWalIndex *pIdx = sqlite3BfWalIndexNew();
  if (pIdx) {
    uint32_t frame_num = (Size % 1000000);
    int rc2 = sqlite3BfWalIndexAddFrame(pIdx, Data, (int)Size, frame_num);

    if (rc2 == BFWAL_OK) {
      /* Query some arbitrary pages */
      for (uint32_t pgno = 1; pgno <= 5 && pgno < Size; pgno++) {
        int count = sqlite3BfWalIndexPageCount(pIdx, pgno);
        for (int i = 0; i < count && i < 10; i++) {
          BfWalRec qrec;
          sqlite3BfWalIndexGet(pIdx, pgno, i, &qrec);
        }
      }

      /* Prune with random frames */
      uint32_t mxFrame = (Size / 2) % 1000000;
      sqlite3BfWalIndexPrune(pIdx, mxFrame);
    }

    sqlite3BfWalIndexFree(pIdx);
  }

  return 0;
}
