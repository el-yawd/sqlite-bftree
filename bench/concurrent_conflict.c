/*
** concurrent_conflict.c -- BEGIN CONCURRENT conflict behaviour, stock vs BF.
**
** Two connections each BEGIN CONCURRENT, read and update one row, commit.
**   SAME row:      a correct engine rejects the second commit (lost update otherwise).
**   DIFFERENT row: both rows sit on one leaf; page-level validation (stock) rejects
**                  the second commit anyway -- a FALSE conflict -- which buffered
**                  record writes avoid.
** 2026-10-13, first run on the merged branch: stock = (reject, reject);
** BF = (BOTH COMMIT -> LOST UPDATE, both commit).  BF needs record-level validation.
**
**   cc -O1 -DSQLITE_OMIT_BF_CACHE -Ibuild -Isrc -o build/conc_stock bench/concurrent_conflict.c build/sqlite3.c -lm
**   cc -O1 -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc -o build/conc_bf bench/concurrent_conflict.c build/sqlite3.c -lm
**   build/conc_bf /tmp/c.db
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
static int ex(sqlite3 *db, const char *z){ char *e=0; int rc=sqlite3_exec(db,z,0,0,&e); if(rc) printf("   [%s] -> %s (%d)\n", z, e, rc); sqlite3_free(e); return rc; }
static long long q(sqlite3 *db, const char *z){ sqlite3_stmt *p; long long r=-1; sqlite3_prepare_v2(db,z,-1,&p,0); if(sqlite3_step(p)==SQLITE_ROW) r=sqlite3_column_int64(p,0); sqlite3_finalize(p); return r; }
static void run(const char *db, int same){
  sqlite3 *A,*B; char b[300];
  for(int i=0;i<3;i++){ snprintf(b,sizeof b,"%s%s",db,i==0?"":i==1?"-wal":"-shm"); remove(b);}
  sqlite3_open(db,&A); sqlite3_open(db,&B);
  ex(A,"PRAGMA journal_mode=wal"); ex(B,"PRAGMA journal_mode=wal");
  ex(A,"CREATE TABLE t(k INTEGER PRIMARY KEY, v INT)");
  ex(A,"WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<1000) INSERT INTO t SELECT x, 0 FROM c");
  ex(A,"PRAGMA wal_checkpoint(TRUNCATE)");
  ex(A,"BEGIN CONCURRENT"); ex(B,"BEGIN CONCURRENT");
  q(A,"SELECT v FROM t WHERE k=500"); q(B, same ? "SELECT v FROM t WHERE k=500" : "SELECT v FROM t WHERE k=501");
  ex(A,"UPDATE t SET v=v+1 WHERE k=500");
  ex(B, same ? "UPDATE t SET v=v+1 WHERE k=500" : "UPDATE t SET v=v+1 WHERE k=501");
  int ra=ex(A,"COMMIT"); int rb=ex(B,"COMMIT");
  if(rb) ex(B,"ROLLBACK");
  printf("  %s row: commitA=%d commitB=%d  v500=%lld v501=%lld\n", same?"SAME":"DIFFERENT", ra, rb,
         q(A,"SELECT v FROM t WHERE k=500"), q(A,"SELECT v FROM t WHERE k=501"));
  sqlite3_close(A); sqlite3_close(B);
}
int main(int c, char**v){ run(v[1],1); run(v[1],0); return 0; }
