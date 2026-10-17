/*
** recover_heap_repro.c -- WAL recovery with a record frame at a block start,
** in a process whose heap is not fresh (2026-10-17).
**
** walIndexRecover() rebuilds each wal-index block in a private scratch buffer
** and relied on walIndexAppend() to zero it at the block's first frame.  A BF
** record frame there is never appended, so the block kept the scratch
** buffer's previous contents: zeros in a fresh process -- which is where every
** crash oracle recovers, so none saw it -- but stale heap bytes in a process
** that has run for a while, and those became bogus page->frame mappings.
** bench/mt_oracle.c crash mode (recovery in the long-lived parent) failed
** nearly every run.
**
** Single connection, no concurrency: after a TRUNCATE checkpoint the next
** commit writes a buffered row and a page, so frame 1 is a record frame and no
** commit precedes the block's first page frame; more commits follow; the process exits without closing (the log survives).  The check
** phase deletes the -shm (forcing recovery), dirties the heap, opens, and
** reads everything back.
**
**   cc -O1 -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
**      -o build/recover_heap bench/recover_heap_repro.c build/sqlite3.c -lm -lpthread
**   build/recover_heap /tmp/rh.db
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void ex(sqlite3 *db, const char *z){
  char *zErr = 0;
  if( sqlite3_exec(db, z, 0, 0, &zErr) ){
    fprintf(stderr, "FAIL: %s: %s\n", z, zErr);
    exit(1);
  }
}

static int makePhase(const char *zDb){
  sqlite3 *db;
  int i;
  char b[200];
  sqlite3_open(zDb, &db);
  ex(db, "PRAGMA journal_mode=wal");
  ex(db, "PRAGMA wal_autocheckpoint=0");
  ex(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT)");
  ex(db, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<3000)"
         " INSERT INTO t SELECT x*2, printf('%.40c','a') FROM c");
  ex(db, "PRAGMA wal_checkpoint(TRUNCATE)");
  /* Frame 1 of the new log generation: a record frame that is NOT a commit --
  ** the transaction also writes pages (a value too big for a mini-page), and
  ** a commit's record frames precede its page frames.  So the block's first
  ** appended page comes before any commit in it, and stock's dead-writer
  ** cleanup (walCleanupHash), which would incidentally scrub the stale
  ** scratch, returns at once (hdr.mxFrame==0). */
  ex(db, "BEGIN");
  ex(db, "INSERT INTO t VALUES(3, 'buffered-three')");
  ex(db, "UPDATE t SET v=printf('%.3000c','z') WHERE k=6000");
  ex(db, "COMMIT");
  /* Then page-image commits (values too big for a mini-page) and more
  ** buffered rows, so the block holds page entries after the record frame. */
  for(i=0; i<60; i++){
    snprintf(b, sizeof(b), "UPDATE t SET v=printf('%%.3000c','%c') WHERE k=%d",
             'b' + i%20, 2 + i*98);
    ex(db, b);
    snprintf(b, sizeof(b), "INSERT INTO t VALUES(%d, 'buf%d')", 5 + i*4, i);
    ex(db, b);
  }
  _exit(0);            /* no close: the log stays, as after a crash */
}

static int checkPhase(const char *zDb){
  sqlite3 *db;
  sqlite3_stmt *p;
  char zShm[600];
  void *apJunk[256];
  int i, bad = 0;
  long long nRow = 0, nLen = 0;
  snprintf(zShm, sizeof(zShm), "%s-shm", zDb);
  remove(zShm);                       /* force walIndexRecover */
  { /* The premise: frame 1 of the log is a record frame (page number field
    ** 0xffffffff), i.e. the first frame of wal-index block 0. */
    char zWal[600]; unsigned char h[4] = {0,0,0,0}; FILE *f;
    snprintf(zWal, sizeof(zWal), "%s-wal", zDb);
    f = fopen(zWal, "rb");
    if( f==0 || fseek(f, 32, SEEK_SET) || fread(h, 1, 4, f)!=4 ){
      fprintf(stderr, "FAIL: no log to recover\n"); return 1;
    }
    fclose(f);
    if( h[0]!=0xff || h[1]!=0xff || h[2]!=0xff || h[3]!=0xff ){
      fprintf(stderr, "FAIL (premise): frame 1 is page %u, not a record frame\n",
              (unsigned)h[0]<<24|(unsigned)h[1]<<16|(unsigned)h[2]<<8|h[3]);
      return 1;
    }
  }
  /* A heap that has been used: freed chunks full of non-zero bytes, of the
  ** sizes the recovery scratch buffer (page + frame header + 32 KiB) is
  ** carved from. */
  for(i=0; i<256; i++){
    /* around page (4096) + frame header (24) + one wal-index block (32768),
    ** plus SQLite's 8-byte size prefix */
    size_t n = 36000 + (size_t)(i%32)*64;
    apJunk[i] = malloc(n);
    if( apJunk[i] ) memset(apJunk[i], 0x5a + (i & 7), n);
  }
  /* Free every other chunk: the survivors pin the heap, so the freed ones stay
  ** in malloc's bins (dirty) instead of being trimmed back to the OS as zeros. */
  for(i=0; i<256; i+=2) free(apJunk[i]);
  if( sqlite3_open(zDb, &db) ){ fprintf(stderr, "FAIL open\n"); return 1; }
  sqlite3_exec(db, "PRAGMA journal_mode=wal", 0, 0, 0);
  if( sqlite3_prepare_v2(db, "SELECT count(*), total(length(v)) FROM t", -1, &p, 0)
   || sqlite3_step(p)!=SQLITE_ROW ){
    fprintf(stderr, "FAIL count: %s\n", sqlite3_errmsg(db));
    return 1;
  }
  nRow = sqlite3_column_int64(p, 0);
  nLen = (long long)sqlite3_column_double(p, 1);
  sqlite3_finalize(p);
  /* 3000 base rows + 1 + 60 buffered; 61 updates of 40 B to 3000 B. */
  if( nRow!=3061 || nLen!=3000LL*40 + 14 + 61LL*(3000-40) + 10*4 + 50*5 ){
    fprintf(stderr, "FAIL: %lld rows, %lld bytes after recovery\n", nRow, nLen);
    bad++;
  }
  if( sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &p, 0)==SQLITE_OK
   && sqlite3_step(p)==SQLITE_ROW
   && strcmp((const char*)sqlite3_column_text(p, 0), "ok") ){
    fprintf(stderr, "FAIL integrity_check: %s\n", (const char*)sqlite3_column_text(p, 0));
    bad++;
  }
  sqlite3_finalize(p);
  sqlite3_close(db);
  if( bad ) return 1;
  printf("OK   recovery with a record frame at a wal-index block start\n");
  return 0;
}

int main(int argc, char **argv){
  const char *zDb = argc>1 ? argv[1] : "recover_heap.db";
  char b[600];
  pid_t pid;
  int st;
  snprintf(b, sizeof(b), "%s", zDb); remove(b);
  snprintf(b, sizeof(b), "%s-wal", zDb); remove(b);
  snprintf(b, sizeof(b), "%s-shm", zDb); remove(b);
  pid = fork();
  if( pid==0 ) return makePhase(zDb);
  waitpid(pid, &st, 0);
  if( !WIFEXITED(st) || WEXITSTATUS(st)!=0 ){ fprintf(stderr, "FAIL make phase\n"); return 1; }
  return checkPhase(zDb);
}
