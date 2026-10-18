/*
** crossgen_repro.c -- C4d (2026-10-18): a reader crosses a log restart from
** the region segments written since its last point, without reseeding.
**
** Two connections on one file.  B buffers rows and checkpoints (a region
** segment); A reads everything (its index is seeded from that segment and
** its cache warm).  B then updates and deletes some of the rows A cached,
** inserts more, and runs a TRUNCATE checkpoint: a second segment, and the
** log restarts.  B writes again in the new generation.  A reads again: it
** must have crossed incrementally (region_crossings 1, no second seed) and
** see exactly B's state; integrity_check then passes.
**
**   cc -O1 -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
**      -o build/crossgen bench/crossgen_repro.c build/sqlite3.c -lm -lpthread
**   build/crossgen /tmp/cg.db
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ex(sqlite3 *db, const char *z){
  char *e = 0;
  if( sqlite3_exec(db, z, 0, 0, &e) ){ fprintf(stderr, "FAIL: %s: %s\n", z, e); exit(1); }
}
static long long q1(sqlite3 *db, const char *z){
  sqlite3_stmt *p; long long v = -1;
  if( sqlite3_prepare_v2(db, z, -1, &p, 0) ){ fprintf(stderr, "FAIL prepare %s\n", z); exit(1); }
  if( sqlite3_step(p)==SQLITE_ROW ) v = sqlite3_column_int64(p, 0);
  sqlite3_finalize(p);
  return v;
}
static long long stat1(sqlite3 *db, const char *zName){
  sqlite3_stmt *p; long long v = -1;
  sqlite3_prepare_v2(db, "PRAGMA bf_cache_stats", -1, &p, 0);
  while( sqlite3_step(p)==SQLITE_ROW ){
    if( strcmp((const char*)sqlite3_column_text(p, 0), zName)==0 ) v = sqlite3_column_int64(p, 1);
  }
  sqlite3_finalize(p);
  return v;
}
int main(int argc, char **argv){
  const char *zDb = argc>1 ? argv[1] : "crossgen.db";
  char b[600]; sqlite3 *A, *B; long long nA, nB, sA, sB, nCross, nSeed;
  snprintf(b, sizeof b, "%s", zDb); remove(b);
  snprintf(b, sizeof b, "%s-wal", zDb); remove(b);
  snprintf(b, sizeof b, "%s-shm", zDb); remove(b);
  sqlite3_open(zDb, &B);
  ex(B, "PRAGMA journal_mode=wal; PRAGMA wal_autocheckpoint=0;"
        "CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT);"
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<4000)"
        "  INSERT INTO t SELECT x*2, printf('%030d', x) FROM c;"
        "PRAGMA wal_checkpoint(TRUNCATE);");
  /* buffered rows, then a region segment */
  ex(B, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<300)"
        "  INSERT INTO t SELECT x*26+1, 'first'||x FROM c;"
        "PRAGMA wal_checkpoint(PASSIVE);");
  sqlite3_open(zDb, &A);
  ex(A, "PRAGMA wal_autocheckpoint=0;");
  nA = q1(A, "SELECT count(*) + total(length(v)) FROM t");  /* seeds A, warms its cache */
  nSeed = stat1(A, "region_seeds");
  /* B changes rows A cached, then a TRUNCATE checkpoint restarts the log */
  ex(B, "UPDATE t SET v='upd'||k WHERE k%52=1;"
        "DELETE FROM t WHERE k%78=1;"
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<200)"
        "  INSERT INTO t SELECT x*26+3, 'second'||x FROM c;"
        "PRAGMA wal_checkpoint(TRUNCATE);");
  /* ... and writes again in the new generation */
  ex(B, "INSERT INTO t VALUES(5, 'new-gen'); UPDATE t SET v='newgen-upd' WHERE k=27;");
  sB = q1(B, "SELECT count(*) + total(length(v)) FROM t");
  nB = q1(B, "SELECT total(k*length(v)) FROM t");
  sA = q1(A, "SELECT count(*) + total(length(v)) FROM t");
  nA = q1(A, "SELECT total(k*length(v)) FROM t");
  nCross = stat1(A, "region_crossings");
  if( sA!=sB || nA!=nB ){
    fprintf(stderr, "FAIL: reader sees %lld/%lld, writer %lld/%lld\n", sA, nA, sB, nB);
    return 1;
  }
  if( nCross!=1 || stat1(A, "region_seeds")!=nSeed ){
    fprintf(stderr, "FAIL: crossings=%lld seeds=%lld (was %lld): not incremental\n",
            nCross, stat1(A, "region_seeds"), nSeed);
    return 1;
  }
  {
    sqlite3_stmt *p; const char *z;
    sqlite3_prepare_v2(A, "PRAGMA integrity_check", -1, &p, 0);
    sqlite3_step(p); z = (const char*)sqlite3_column_text(p, 0);
    if( strcmp(z, "ok") ){ fprintf(stderr, "FAIL integrity: %s\n", z); return 1; }
    sqlite3_finalize(p);
  }
  sqlite3_close(A); sqlite3_close(B);
  printf("OK   reader crossed a log restart incrementally (%lld rows+bytes)\n", sA);
  return 0;
}
