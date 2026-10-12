/*
** Fuzzer for BF-Tree WAL recovery and replay.
**
** Builds a small database with buffered operations, then mutates the WAL
** and attempts to recover/replay. Tests:
** 1. Robustness of recovery path with corrupted WAL frames
** 2. Integrity of the recovered database
** 3. Ability to handle torn/truncated frames
**
** Must never crash, leak, or enter an invalid state.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "sqlite3.h"

static char fuzz_db_path[256];
static char fuzz_wal_path[256];

static void fuzz_cleanup(void) {
  unlink(fuzz_db_path);
  unlink(fuzz_wal_path);
}

extern int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  if (Size < 16) return 0;

  /* Create temp filenames */
  static int run_count = 0;
  run_count++;
  snprintf(fuzz_db_path, sizeof(fuzz_db_path), "/tmp/fuzz_bf_db_%d.db", run_count);
  snprintf(fuzz_wal_path, sizeof(fuzz_wal_path), "/tmp/fuzz_bf_db_%d.db-wal", run_count);

  fuzz_cleanup();

  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(fuzz_db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  if (rc != SQLITE_OK) {
    fuzz_cleanup();
    return 0;
  }

  /* Enable WAL mode and BF caching */
  sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
  sqlite3_exec(db, "PRAGMA bf_cache_size=262144;", NULL, NULL, NULL);
  sqlite3_exec(db, "PRAGMA bf_promotion_rate=100;", NULL, NULL, NULL);

  /* Create a simple rowid table */
  sqlite3_exec(db, "CREATE TABLE t1(x INTEGER PRIMARY KEY, y TEXT);", NULL, NULL, NULL);

  /* Insert some data with buffering */
  sqlite3_stmt *stmt = NULL;
  rc = sqlite3_prepare_v2(db, "INSERT INTO t1(x, y) VALUES(?, ?);", -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    for (int i = 1; i <= 10; i++) {
      sqlite3_bind_int(stmt, 1, i);
      sqlite3_bind_text(stmt, 2, "test", -1, SQLITE_STATIC);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
  }

  /* Commit to get the WAL populated */
  sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
  sqlite3_close(db);

  /* Now mutate the WAL file using the fuzzer input */
  FILE *wal = fopen(fuzz_wal_path, "r+b");
  if (wal == NULL) {
    fuzz_cleanup();
    return 0;
  }

  /* Get WAL size */
  fseek(wal, 0, SEEK_END);
  long wal_size = ftell(wal);
  fseek(wal, 0, SEEK_SET);

  /* Read the WAL into memory */
  uint8_t *wal_data = (uint8_t *)malloc(wal_size);
  if (wal_data == NULL) {
    fclose(wal);
    fuzz_cleanup();
    return 0;
  }

  size_t nread = fread(wal_data, 1, wal_size, wal);
  fclose(wal);

  if (nread != (size_t)wal_size) {
    free(wal_data);
    fuzz_cleanup();
    return 0;
  }

  /* Mutate the WAL: use fuzzer input to flip bits/bytes after the 32-byte header */
  if (wal_size > 32) {
    for (size_t i = 0; i < Size && i < (size_t)(wal_size - 32); i++) {
      size_t pos = 32 + i;
      if (pos < (size_t)wal_size) {
        /* XOR with fuzzer byte to create mutations */
        wal_data[pos] ^= Data[i];
      }
    }
  }

  /* Write back the mutated WAL */
  wal = fopen(fuzz_wal_path, "wb");
  if (wal != NULL) {
    fwrite(wal_data, 1, wal_size, wal);
    fclose(wal);

    /* Now try to open and recover */
    db = NULL;
    rc = sqlite3_open_v2(fuzz_db_path, &db, SQLITE_OPEN_READWRITE, NULL);

    if (rc == SQLITE_OK && db != NULL) {
      /* Set up BF caching BEFORE any read */
      sqlite3_exec(db, "PRAGMA bf_cache_size=262144;", NULL, NULL, NULL);

      /* Try various operations that might trigger recovery */
      sqlite3_exec(db, "SELECT COUNT(*) FROM t1;", NULL, NULL, NULL);
      sqlite3_exec(db, "PRAGMA integrity_check;", NULL, NULL, NULL);
      sqlite3_exec(db, "PRAGMA wal_checkpoint(PASSIVE);", NULL, NULL, NULL);

      /* Try a write to test consistency */
      sqlite3_exec(db, "INSERT INTO t1(x, y) VALUES(999, 'recovered');", NULL, NULL, NULL);
      sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

      /* Another integrity check */
      sqlite3_exec(db, "PRAGMA integrity_check;", NULL, NULL, NULL);

      sqlite3_close(db);
    }
  }

  free(wal_data);
  fuzz_cleanup();

  return 0;
}
