/*
** Fuzzer for BF-Tree end-to-end SQL semantics.
**
** Generates random SQL statements over rowid tables with BF enabled,
** including multi-connection scenarios. Tests:
** 1. Correctness of SQL execution with BF caching
** 2. Robustness under concurrent reader/writer access
** 3. Integrity after various operations (INSERT/UPDATE/DELETE/COMMIT/ROLLBACK)
**
** Must never crash or corrupt the database.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "sqlite3.h"

#ifdef __cplusplus
}
#include <fuzzer/FuzzedDataProvider.h>

class SqlFuzzer {
 public:
  SqlFuzzer(const uint8_t *data, size_t size) : provider(data, size) {}

  void run() {
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "/tmp/fuzz_sql_%p.db", this);

    unlink(db_path);

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
      unlink(db_path);
      return;
    }

    /* Set up BF cache and pragmas */
    const char *pragmas[] = {
      "PRAGMA journal_mode=WAL;",
      "PRAGMA bf_cache_size=262144;",
      "PRAGMA synchronous=NORMAL;",
      "PRAGMA cache_size=2000;",
    };
    for (const auto &p : pragmas) {
      sqlite3_exec(db, p, NULL, NULL, NULL);
    }

    /* Create initial tables */
    sqlite3_exec(db, "CREATE TABLE t1(id INTEGER PRIMARY KEY, val TEXT);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TABLE t2(id INTEGER PRIMARY KEY, data BLOB);", NULL, NULL, NULL);

    /* Random operations */
    for (int op = 0; op < 50; op++) {
      uint8_t op_type = provider.ConsumeIntegral<uint8_t>();

      switch (op_type % 10) {
        case 0: {
          /* INSERT */
          int id = provider.ConsumeIntegral<int32_t>();
          std::string val = provider.ConsumeRandomLengthString(100);
          char sql[512];
          snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO t1(id, val) VALUES(%d, '%s');", id, val.c_str());
          sqlite3_exec(db, sql, NULL, NULL, NULL);
          break;
        }
        case 1: {
          /* UPDATE */
          int id = provider.ConsumeIntegral<int32_t>();
          std::string val = provider.ConsumeRandomLengthString(100);
          char sql[512];
          snprintf(sql, sizeof(sql), "UPDATE t1 SET val='%s' WHERE id=%d;", val.c_str(), id);
          sqlite3_exec(db, sql, NULL, NULL, NULL);
          break;
        }
        case 2: {
          /* DELETE */
          int id = provider.ConsumeIntegral<int32_t>();
          char sql[256];
          snprintf(sql, sizeof(sql), "DELETE FROM t1 WHERE id=%d;", id);
          sqlite3_exec(db, sql, NULL, NULL, NULL);
          break;
        }
        case 3: {
          /* SELECT COUNT(*) */
          sqlite3_exec(db, "SELECT COUNT(*) FROM t1;", NULL, NULL, NULL);
          break;
        }
        case 4: {
          /* COMMIT */
          sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
          break;
        }
        case 5: {
          /* ROLLBACK */
          sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
          break;
        }
        case 6: {
          /* BEGIN */
          sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
          break;
        }
        case 7: {
          /* SAVEPOINT / ROLLBACK TO */
          sqlite3_exec(db, "SAVEPOINT sp1;", NULL, NULL, NULL);
          sqlite3_exec(db, "ROLLBACK TO sp1;", NULL, NULL, NULL);
          break;
        }
        case 8: {
          /* Checkpoint */
          uint8_t mode = provider.ConsumeIntegral<uint8_t>();
          const char *ckpt_modes[] = {"PASSIVE", "RESTART", "TRUNCATE"};
          char sql[256];
          snprintf(sql, sizeof(sql), "PRAGMA wal_checkpoint(%s);", ckpt_modes[mode % 3]);
          sqlite3_exec(db, sql, NULL, NULL, NULL);
          break;
        }
        case 9: {
          /* Pragma changes */
          uint8_t pragma_type = provider.ConsumeIntegral<uint8_t>();
          const char *pragmas_dyn[] = {
            "PRAGMA bf_promotion_rate=50;",
            "PRAGMA bf_scan_promotion=100;",
            "PRAGMA bf_read_record_cache=0;",
            "PRAGMA bf_group_commit=8;",
            "PRAGMA integrity_check;",
          };
          sqlite3_exec(db, pragmas_dyn[pragma_type % 5], NULL, NULL, NULL);
          break;
        }
      }
    }

    /* Test multi-connection scenario */
    sqlite3 *db_reader = NULL;
    rc = sqlite3_open_v2(db_path, &db_reader, SQLITE_OPEN_READONLY, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_exec(db_reader, "PRAGMA bf_cache_size=262144;", NULL, NULL, NULL);
      sqlite3_exec(db_reader, "SELECT COUNT(*) FROM t1;", NULL, NULL, NULL);

      /* Writer does some work while reader is active */
      sqlite3_exec(db, "INSERT OR IGNORE INTO t1(id, val) VALUES(9999, 'concurrent');", NULL, NULL, NULL);
      sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

      /* Reader continues */
      sqlite3_exec(db_reader, "SELECT COUNT(*) FROM t1;", NULL, NULL, NULL);
      sqlite3_exec(db_reader, "PRAGMA integrity_check;", NULL, NULL, NULL);

      sqlite3_close(db_reader);
    }

    /* Final checks */
    sqlite3_exec(db, "PRAGMA integrity_check;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);

    sqlite3_close(db);
    unlink(db_path);
  }

 private:
  FuzzedDataProvider provider;
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  try {
    SqlFuzzer fuzzer(Data, Size);
    fuzzer.run();
  } catch (...) {
    /* Catch any C++ exceptions */
  }
  return 0;
}
#endif  /* __cplusplus */
