/*
** fuzz_sql_mconn.c -- libFuzzer harness: BF-enabled SQL over ONE writer and ONE
** reader connection on the same file (separate pagers, private caches).
**
** The input is a byte stream: the first byte picks a pragma set (ring size,
** promotion, read mode, adaptive admission, group commit), every following
** byte (plus operands) is one operation on the writer or the reader.  Finds
** crashes, ASan/UBSan reports and SQLITE_DEBUG asserts; it also checks two
** cheap invariants that need no model: count(*) equals the number of rows a
** full scan returns, on both connections, and integrity_check at the end.
** Answers are checked properly by bench/mconn_oracle.c; this harness hunts
** for inputs that break the engine.
**
** Build (from the repo root, after `cd build && make sqlite3`):
**   clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
**     -DSQLITE_DEBUG -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
**     test/fuzz/fuzz_sql_mconn.c build/sqlite3.c -o test/fuzz/fuzz_sql_mconn -lm
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct In { const unsigned char *a; size_t n, i; } In;
static unsigned u8(In *p){ return p->i < p->n ? p->a[p->i++] : 0; }
static unsigned u16(In *p){ return u8(p) | (u8(p)<<8); }

static const char *azPrag[] = {
  "",
  "PRAGMA bf_cache_size=262144;",
  "PRAGMA bf_cache_size=262144; PRAGMA bf_promotion_rate=100; PRAGMA bf_scan_promotion=100;",
  "PRAGMA bf_cache_size=262144; PRAGMA bf_promotion_rate=100; PRAGMA bf_read_record_cache=0;",
  "PRAGMA bf_cache_size=262144; PRAGMA bf_promotion_rate=100; PRAGMA bf_adaptive_full=2;",
  "PRAGMA bf_cache_size=131072; PRAGMA bf_promotion_rate=100;",
  "PRAGMA bf_group_commit=8;",
  "PRAGMA bf_cache_size=262144; PRAGMA wal_autocheckpoint=50;",
};

static void ex(sqlite3 *db, const char *z){ sqlite3_exec(db, z, 0, 0, 0); }

/* count(*) must equal the rows a scan returns, in the same snapshot. */
static void checkCount(sqlite3 *db){
  sqlite3_stmt *p;
  long long nCount = -1, nScan = 0;
  ex(db, "BEGIN");
  if( sqlite3_prepare_v2(db, "SELECT count(*) FROM t", -1, &p, 0)==SQLITE_OK ){
    if( sqlite3_step(p)==SQLITE_ROW ) nCount = sqlite3_column_int64(p, 0);
    sqlite3_finalize(p);
  }
  if( sqlite3_prepare_v2(db, "SELECT k FROM t", -1, &p, 0)==SQLITE_OK ){
    int rc;
    while( (rc = sqlite3_step(p))==SQLITE_ROW ) nScan++;
    sqlite3_finalize(p);
    if( rc!=SQLITE_DONE ) nScan = nCount;   /* errors (FULL/BUSY) are not findings */
  }
  ex(db, "COMMIT");
  if( nCount>=0 && nCount!=nScan ){
    fprintf(stderr, "INVARIANT: count(*)=%lld but scan=%lld\n", nCount, nScan);
    abort();
  }
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size){
  In in = { data, size, 0 };
  char zDb[64];
  sqlite3 *W = 0, *R = 0;
  int iPrag, nOp = 0, wTxn = 0, rTxn = 0;
  static char aVal[512];
  if( size<2 ) return 0;
  snprintf(zDb, sizeof(zDb), "/tmp/fz_mconn_%d.db", (int)getpid());
  { char b[80]; unlink(zDb);
    snprintf(b, sizeof(b), "%s-wal", zDb); unlink(b);
    snprintf(b, sizeof(b), "%s-shm", zDb); unlink(b); }
  iPrag = u8(&in) % (int)(sizeof(azPrag)/sizeof(azPrag[0]));
  sqlite3_open_v2(zDb, &W, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_PRIVATECACHE, 0);
  ex(W, "PRAGMA journal_mode=wal;"); ex(W, azPrag[iPrag]);
  ex(W, "CREATE TABLE t(k INTEGER PRIMARY KEY, v BLOB)");
  ex(W, "CREATE TABLE u(k INTEGER PRIMARY KEY, a INT, b TEXT)");
  ex(W, "CREATE INDEX ua ON u(a)");
  sqlite3_open_v2(zDb, &R, SQLITE_OPEN_READWRITE|SQLITE_OPEN_PRIVATECACHE, 0);
  ex(R, "PRAGMA journal_mode=wal;"); ex(R, azPrag[iPrag]);
  sqlite3_busy_timeout(W, 0); sqlite3_busy_timeout(R, 0);

  while( in.i < in.n && nOp++ < 400 ){
    unsigned op = u8(&in);
    int k = (int)(u16(&in) % 2000) + 1;
    int len = (int)(u8(&in) % 3) == 0 ? (int)(u16(&in) % 500) : (int)(u8(&in) % 120);
    char z[1024];
    memset(aVal, 'a' + (op % 26), sizeof(aVal));
    switch( op % 24 ){
      case 0: case 1: case 2:
        snprintf(z, sizeof(z), "INSERT OR REPLACE INTO t VALUES(%d, zeroblob(%d))", k, len);
        ex(W, z); break;
      case 3: case 4:
        snprintf(z, sizeof(z), "UPDATE t SET v=randomblob(%d) WHERE k=%d", len, k);
        ex(W, z); break;
      case 5:
        snprintf(z, sizeof(z), "DELETE FROM t WHERE k=%d", k); ex(W, z); break;
      case 6:
        snprintf(z, sizeof(z), "DELETE FROM t WHERE k BETWEEN %d AND %d", k, k+(len%40));
        ex(W, z); break;
      case 7:
        snprintf(z, sizeof(z), "INSERT OR REPLACE INTO u VALUES(%d,%d,'%.*s')",
                 k, len%50, len%200, aVal);
        ex(W, z); break;
      case 8:
        snprintf(z, sizeof(z), "UPDATE u SET b='%.*s' WHERE a=%d", len%200, aVal, len%50);
        ex(W, z); break;
      case 9:  if( !wTxn ){ ex(W, "BEGIN"); wTxn = 1; } break;
      case 10: if( wTxn ){ ex(W, "COMMIT"); wTxn = 0; } break;
      case 11: if( wTxn ){ ex(W, "ROLLBACK"); wTxn = 0; } break;
      case 12: ex(W, "SAVEPOINT s"); break;
      case 13: ex(W, "ROLLBACK TO s"); break;
      case 14: ex(W, "RELEASE s"); break;
      case 15: {
        static const char *az[] = { "PRAGMA wal_checkpoint(PASSIVE)",
          "PRAGMA wal_checkpoint(RESTART)", "PRAGMA wal_checkpoint(TRUNCATE)" };
        if( !wTxn ) ex(W, az[len%3]);
        break;
      }
      case 16: if( !rTxn ){ ex(R, "BEGIN"); rTxn = 1; } break;
      case 17: if( rTxn ){ ex(R, "COMMIT"); rTxn = 0; } break;
      case 18: case 19: {
        sqlite3 *db = (op & 32) ? W : R;
        snprintf(z, sizeof(z), "SELECT sum(length(v)) FROM t WHERE k BETWEEN %d AND %d%s",
                 k, k+len, (op & 64) ? "" : "");
        ex(db, z);
        snprintf(z, sizeof(z), "SELECT k FROM t WHERE k<=%d ORDER BY k DESC LIMIT %d",
                 k, 1+len%30);
        ex(db, z);
        break;
      }
      case 20: {
        sqlite3 *db = (op & 32) ? W : R;
        snprintf(z, sizeof(z), "SELECT v FROM t WHERE k=%d", k); ex(db, z);
        snprintf(z, sizeof(z), "SELECT b FROM u WHERE a=%d", len%50); ex(db, z);
        break;
      }
      case 21: ex((op & 32) ? W : R, "SELECT count(*) FROM t; SELECT max(k), min(k) FROM t"); break;
      case 22: if( !wTxn && !rTxn ) checkCount((op & 32) ? W : R); break;
      case 23: {   /* reopen the reader: a fresh cache joining an existing WAL */
        if( rTxn ){ ex(R, "COMMIT"); rTxn = 0; }
        sqlite3_close(R);
        sqlite3_open_v2(zDb, &R, SQLITE_OPEN_READWRITE|SQLITE_OPEN_PRIVATECACHE, 0);
        ex(R, "PRAGMA journal_mode=wal;"); ex(R, azPrag[iPrag]);
        sqlite3_busy_timeout(R, 0);
        break;
      }
    }
  }
  if( wTxn ) ex(W, "COMMIT");
  if( rTxn ) ex(R, "COMMIT");
  checkCount(W);
  checkCount(R);
  {
    sqlite3_stmt *p;
    if( sqlite3_prepare_v2(R, "PRAGMA integrity_check", -1, &p, 0)==SQLITE_OK ){
      if( sqlite3_step(p)==SQLITE_ROW ){
        const char *zr = (const char*)sqlite3_column_text(p, 0);
        if( zr && strcmp(zr, "ok")!=0 ){
          fprintf(stderr, "INVARIANT: integrity_check: %s\n", zr);
          abort();
        }
      }
      sqlite3_finalize(p);
    }
  }
  sqlite3_close(R);
  sqlite3_close(W);
  return 0;
}
