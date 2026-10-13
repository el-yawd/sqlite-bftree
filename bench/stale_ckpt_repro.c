/*
** stale_ckpt_repro.c -- a checkpoint run by a connection whose record cache is
** stale must not lose another connection's committed buffered rows.
**
** Found 2026-10-13 (bench/mwriter_oracle.c, wal_autocheckpoint=50).
** bfCheckpointMaterialize decided "nothing buffered" from the checkpointing
** connection's cache BEFORE beginning the write transaction that brings that
** cache to the newest snapshot.  A reader that last read before the writer's
** commits saw an empty cache, skipped materialisation, and its close-time
** TRUNCATE checkpoint reset the log -- erasing the writer's record frames.
**
** One writer, one reader, no BEGIN CONCURRENT.  Exit 0 = OK.
**   cc -O1 -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
**      -o build/stale_ckpt bench/stale_ckpt_repro.c build/sqlite3.c -lm -lpthread
**   build/stale_ckpt /tmp/s.db
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>

static void ex(sqlite3 *db, const char *z){
  char *e = 0;
  if( sqlite3_exec(db, z, 0, 0, &e) ){
    fprintf(stderr, "ERR %s: %s\n", z, e);
    exit(2);
  }
}
static long long q(sqlite3 *db, const char *z){
  sqlite3_stmt *p;
  long long r = -1;
  sqlite3_prepare_v2(db, z, -1, &p, 0);
  if( sqlite3_step(p)==SQLITE_ROW ) r = sqlite3_column_int64(p, 0);
  sqlite3_finalize(p);
  return r;
}

int main(int argc, char **argv){
  const char *zDb = argc>1 ? argv[1] : "stale_ckpt.db";
  sqlite3 *W, *R;
  char b[512];
  long long n;
  int i;
  for(i=0; i<3; i++){
    snprintf(b, sizeof(b), "%s%s", zDb, i==0 ? "" : i==1 ? "-wal" : "-shm");
    remove(b);
  }
  sqlite3_open(zDb, &W);
  ex(W, "PRAGMA journal_mode=wal");
  ex(W, "PRAGMA wal_autocheckpoint=0");
  ex(W, "CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT)");
  ex(W, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<500)"
        " INSERT INTO t SELECT x*2, 'base' FROM c");
  ex(W, "PRAGMA wal_checkpoint(TRUNCATE)");   /* everything in base, log empty */

  sqlite3_open(zDb, &R);
  ex(R, "PRAGMA journal_mode=wal");
  if( q(R, "SELECT count(*) FROM t")!=500 ){ fprintf(stderr, "setup\n"); return 2; }

  /* The writer buffers new rows and commits them: they live in record frames
  ** and in the writer's cache only.  The reader does not read again. */
  for(i=0; i<50; i++){
    snprintf(b, sizeof(b), "INSERT INTO t VALUES(%d, 'buffered')", i*2+1);
    ex(W, b);
  }

  /* The reader checkpoints with its stale cache, as sqlite3_close does. */
  ex(R, "PRAGMA wal_checkpoint(TRUNCATE)");
  sqlite3_close(R);

  /* A third connection, opened now, sees what a crash of the writer would
  ** leave behind: the db file plus whatever log survived the checkpoint.  The
  ** writer is still open; its cache would hide the loss from itself (and its
  ** close-time checkpoint would re-materialise most of the rows). */
  { sqlite3 *X;
    sqlite3_open(zDb, &X);
    n = q(X, "SELECT count(*) FROM t");
    sqlite3_close(X);
  }
  sqlite3_close(W);
  if( n!=550 ){
    printf("FAIL: a new connection sees %lld rows, want 550 "
           "(the writer's committed rows were erased)\n", n);
    return 1;
  }
  printf("OK   550 rows after a stale-cache checkpoint\n");
  return 0;
}
