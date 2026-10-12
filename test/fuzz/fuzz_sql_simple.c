/*
** Fuzzer for BF-Tree end-to-end SQL semantics.
** Pure C version to avoid C++ incompatibilities with sqlite3.c
**
** Generates random SQL statements over rowid tables with BF enabled.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "sqlite3.h"

#define MIN(a,b) ((a)<(b)?(a):(b))

typedef struct {
  const uint8_t *data;
  size_t pos;
  size_t size;
} FuzzInput;

static uint32_t fuzz_u32(FuzzInput *p) {
  if (p->pos + 4 > p->size) return 0;
  uint32_t v = ((uint32_t)p->data[p->pos] << 24)
             | ((uint32_t)p->data[p->pos+1] << 16)
             | ((uint32_t)p->data[p->pos+2] << 8)
             | ((uint32_t)p->data[p->pos+3]);
  p->pos += 4;
  return v;
}

static uint8_t fuzz_u8(FuzzInput *p) {
  if (p->pos >= p->size) return 0;
  return p->data[p->pos++];
}

static const char *fuzz_string(FuzzInput *p, char *buf, size_t maxlen) {
  if (p->pos >= p->size) {
    buf[0] = 0;
    return buf;
  }
  size_t requested = fuzz_u8(p) % maxlen;
  size_t available = p->size - p->pos;
  size_t len = MIN(requested, available);
  if (len > 0 && len <= maxlen - 1) {
    memcpy(buf, p->data + p->pos, len);
    p->pos += len;
  }
  buf[len] = 0;
  return buf;
}

extern int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  if (Size < 16) return 0;

  FuzzInput input = {Data, 0, Size};
  char temp_db[256];
  snprintf(temp_db, sizeof(temp_db), "/tmp/fuzz_bf_sql_%p.db", Data);
  unlink(temp_db);

  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(temp_db, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  if (rc != SQLITE_OK || !db) {
    unlink(temp_db);
    return 0;
  }

  /* Set up BF cache */
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
  sqlite3_exec(db, "PRAGMA bf_cache_size=262144;", NULL, NULL, NULL);
  sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

  /* Create tables */
  sqlite3_exec(db, "CREATE TABLE t1(id INTEGER PRIMARY KEY, val TEXT);", NULL, NULL, NULL);
  sqlite3_exec(db, "CREATE TABLE t2(id INTEGER PRIMARY KEY, data BLOB);", NULL, NULL, NULL);

  /* Random operations */
  char buf1[256];
  for (int op_count = 0; op_count < 30; op_count++) {
    uint8_t op_type = fuzz_u8(&input);
    int32_t id = (int32_t)fuzz_u32(&input);
    const char *val = fuzz_string(&input, buf1, sizeof(buf1));

    switch (op_type % 8) {
      case 0: {  /* INSERT */
        char sql[512];
        snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO t1(id, val) VALUES(%d, '%s');",
                 id % 100000, val);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        break;
      }
      case 1: {  /* UPDATE */
        char sql[512];
        snprintf(sql, sizeof(sql), "UPDATE t1 SET val='%s' WHERE id=%d;", val, id % 100000);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        break;
      }
      case 2: {  /* DELETE */
        char sql[256];
        snprintf(sql, sizeof(sql), "DELETE FROM t1 WHERE id=%d;", id % 100000);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        break;
      }
      case 3: {  /* SELECT COUNT */
        sqlite3_exec(db, "SELECT COUNT(*) FROM t1;", NULL, NULL, NULL);
        break;
      }
      case 4: {  /* COMMIT */
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        break;
      }
      case 5: {  /* ROLLBACK */
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        break;
      }
      case 6: {  /* Checkpoint */
        sqlite3_exec(db, "PRAGMA wal_checkpoint(PASSIVE);", NULL, NULL, NULL);
        break;
      }
      case 7: {  /* Pragma */
        uint8_t p_type = fuzz_u8(&input);
        switch (p_type % 2) {
          case 0:
            sqlite3_exec(db, "PRAGMA bf_promotion_rate=50;", NULL, NULL, NULL);
            break;
          case 1:
            sqlite3_exec(db, "PRAGMA integrity_check;", NULL, NULL, NULL);
            break;
        }
        break;
      }
    }

    if (input.pos >= input.size) break;
  }

  /* Final checks */
  sqlite3_exec(db, "PRAGMA integrity_check;", NULL, NULL, NULL);
  sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
  sqlite3_close(db);

  unlink(temp_db);
  return 0;
}
