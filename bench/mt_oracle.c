/*
** mt_oracle.c -- THREADED BEGIN CONCURRENT invariant oracle (C2b, 2026-10-17).
**
** mwriter_oracle.c interleaves K connections on ONE thread, so it checks the
** serialisability logic exactly but never runs two commits, a commit and a
** checkpoint, or a commit and a snapshot catch-up at the same time.  This one
** does: T threads, each with its own connection (private cache, as the owner's
** model has it), run BEGIN CONCURRENT transactions for real, retrying a
** refused commit.  It checks invariants instead of a model:
**
**   1. Bank transfers.  acct(id, bal) starts with every balance at 1000.  A
**      transfer reads two balances and writes both, moving an amount between
**      them.  Any lost update changes the total, so every read-only
**      transaction that sums the table -- taken at random by every thread --
**      must see NACCT*1000, and so must the final state.
**   2. A commit log.  Each committed transfer also inserts hist(id=thread
**      *1e9+seq, ...) with an explicit rowid; each thread remembers which of
**      its seqs committed.  At the end hist must hold exactly those rows, and
**      every balance must equal 1000 plus what the log says moved into it.
**   3. Recovery.  The final checks run again on a fresh connection after all
**      threads closed theirs, i.e. after the record cache was rebuilt from the
**      log (and, at close, the TRUNCATE checkpoint).
**
** A small fraction of operations run PRAGMA wal_checkpoint (PASSIVE), so the
** checkpoint materialise races with concurrent commits.
**
** Build (BEGIN CONCURRENT compiled in, threadsafe):
**   cc -O1 -g -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
**      -o build/mt_bf bench/mt_oracle.c build/sqlite3.c -lm -lpthread
**   cc -O1 -g -DSQLITE_OMIT_BF_CACHE -Ibuild -Isrc \
**      -o build/mt_stock bench/mt_oracle.c build/sqlite3.c -lm -lpthread
** Usage:
**   mt_bf DB SEED NTHREADS NTXN_PER_THREAD ["PRAGMAS"]
** Validate on the stock build first.
*/
#include "sqlite3.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NACCT      400
#define MAXTHR     16
#define HIST_BASE  1000000000LL

static const char *zDb;
static const char *zPragmas = "";
static int nThr, nTxnPer;
static unsigned long long seed0;
static volatile int bFail;

typedef struct Thr {
  int id;
  sqlite3 *db;
  unsigned long long rng;
  char *aDone;              /* aDone[seq]: transfer seq committed */
  int *aFrom, *aTo, *aAmt;  /* what transfer seq did */
  long nCommit, nRetry, nReadCheck, nCkpt;
  char zErr[256];
} Thr;
static Thr aThr[MAXTHR];

static unsigned rnd(Thr *t){
  t->rng ^= t->rng << 13; t->rng ^= t->rng >> 7; t->rng ^= t->rng << 17;
  return (unsigned)(t->rng >> 11);
}

/* MT_LOG=1: print sqlite3_log() (SQLITE_CORRUPT_BKPT names its source line). */
static void logCb(void *pArg, int iErr, const char *zMsg){
  (void)pArg;
  if( (iErr&0xff)!=SQLITE_SCHEMA ) fprintf(stderr, "LOG %d: %s\n", iErr, zMsg);
}

static void fail(Thr *t, const char *zWhat, sqlite3 *db){
  if( t ){
    snprintf(t->zErr, sizeof(t->zErr), "thread %d: %s: %s", t->id, zWhat,
             db ? sqlite3_errmsg(db) : "");
  }else{
    fprintf(stderr, "FAIL %s: %s\n", zWhat, db ? sqlite3_errmsg(db) : "");
  }
  bFail = 1;
}

/* Run zSql; returns the result code of the last step. */
static int execStep(sqlite3 *db, const char *zSql){
  return sqlite3_exec(db, zSql, 0, 0, 0);
}

static int getBal(sqlite3 *db, int id, long long *pBal){
  sqlite3_stmt *p;
  int rc = sqlite3_prepare_v2(db, "SELECT bal FROM acct WHERE id=?", -1, &p, 0);
  if( rc ) return rc;
  sqlite3_bind_int(p, 1, id);
  rc = sqlite3_step(p);
  if( rc==SQLITE_ROW ){ *pBal = sqlite3_column_int64(p, 0); rc = SQLITE_OK; }
  else if( rc==SQLITE_DONE ){ rc = SQLITE_CORRUPT; }   /* account vanished */
  sqlite3_finalize(p);
  return rc;
}

static int setBal(sqlite3 *db, int id, long long bal){
  sqlite3_stmt *p;
  int rc = sqlite3_prepare_v2(db, "UPDATE acct SET bal=? WHERE id=?", -1, &p, 0);
  if( rc ) return rc;
  sqlite3_bind_int64(p, 1, bal);
  sqlite3_bind_int(p, 2, id);
  rc = sqlite3_step(p);
  sqlite3_finalize(p);
  if( rc==SQLITE_DONE && sqlite3_changes(db)!=1 ) return SQLITE_CORRUPT;
  return rc==SQLITE_DONE ? SQLITE_OK : rc;
}

static int addHist(sqlite3 *db, long long hid, int from, int to, int amt){
  sqlite3_stmt *p;
  int rc = sqlite3_prepare_v2(db,
      "INSERT INTO hist(id, a, b, amt) VALUES(?,?,?,?)", -1, &p, 0);
  if( rc ) return rc;
  sqlite3_bind_int64(p, 1, hid);
  sqlite3_bind_int(p, 2, from);
  sqlite3_bind_int(p, 3, to);
  sqlite3_bind_int(p, 4, amt);
  rc = sqlite3_step(p);
  sqlite3_finalize(p);
  return rc==SQLITE_DONE ? SQLITE_OK : rc;
}

/* Sum of balances and row count, in whatever transaction is open. */
static int sumBal(sqlite3 *db, long long *pSum, long long *pN){
  sqlite3_stmt *p;
  int rc = sqlite3_prepare_v2(db, "SELECT total(bal), count(*) FROM acct", -1, &p, 0);
  if( rc ) return rc;
  rc = sqlite3_step(p);
  if( rc==SQLITE_ROW ){
    *pSum = (long long)sqlite3_column_double(p, 0);
    *pN = sqlite3_column_int64(p, 1);
    rc = SQLITE_OK;
  }
  sqlite3_finalize(p);
  return rc;
}

/* Is this a "try again" outcome of BEGIN CONCURRENT? */
static int isRetry(int rc){
  return (rc&0xff)==SQLITE_BUSY || (rc&0xff)==SQLITE_LOCKED;
}

static void *thrMain(void *pArg){
  Thr *t = (Thr*)pArg;
  int seq, rc;
  for(seq=0; seq<nTxnPer && !bFail; seq++){
    int from = 1 + rnd(t)%NACCT, to = 1 + rnd(t)%NACCT, amt = 1 + rnd(t)%50;
    unsigned r = rnd(t)%100;
    if( to==from ) to = 1 + (to % NACCT);
    if( r<3 ){
      /* A checkpoint racing the other threads' commits. */
      rc = execStep(t->db, "PRAGMA wal_checkpoint");
      if( rc!=SQLITE_OK && !isRetry(rc) ){ fail(t, "checkpoint", t->db); break; }
      t->nCkpt++;
    }else if( r<15 ){
      /* A read-only snapshot must always see the invariant total. */
      long long sum = 0, n = 0;
      rc = execStep(t->db, "BEGIN");
      if( rc==SQLITE_OK ) rc = sumBal(t->db, &sum, &n);
      execStep(t->db, "COMMIT");
      if( rc==SQLITE_OK && (sum!=(long long)NACCT*1000 || n!=NACCT) ){
        char z[160];
        snprintf(z, sizeof(z), "snapshot read: total %lld over %lld rows "
                 "(want %d over %d)", sum, n, NACCT*1000, NACCT);
        fail(t, z, 0);
        break;
      }
      if( rc!=SQLITE_OK && !isRetry(rc) ){ fail(t, "snapshot read", t->db); break; }
      t->nReadCheck++;
    }
    t->aFrom[seq] = from; t->aTo[seq] = to; t->aAmt[seq] = amt;
    for(;;){
      long long ba = 0, bb = 0;
      if( bFail ) break;
      rc = execStep(t->db, "BEGIN CONCURRENT");
      if( rc ){ if( isRetry(rc) ){ t->nRetry++; continue; } fail(t, "begin", t->db); break; }
      rc = getBal(t->db, from, &ba);
      if( rc==SQLITE_OK ) rc = getBal(t->db, to, &bb);
      if( rc==SQLITE_OK ) rc = setBal(t->db, from, ba - amt);
      if( rc==SQLITE_OK ) rc = setBal(t->db, to, bb + amt);
      if( rc==SQLITE_OK ) rc = addHist(t->db, (long long)t->id*HIST_BASE + seq,
                                       from, to, amt);
      if( rc==SQLITE_OK ) rc = execStep(t->db, "COMMIT");
      if( rc==SQLITE_OK ){
        t->aDone[seq] = 1;
        t->nCommit++;
        break;
      }
      if( sqlite3_get_autocommit(t->db)==0 ) execStep(t->db, "ROLLBACK");
      if( isRetry(rc) ){ t->nRetry++; continue; }
      fail(t, "transfer", t->db);
      break;
    }
  }
  return 0;
}

/* The end-state checks, on any connection. */
static int finalCheck(sqlite3 *db, const char *zWhen){
  long long aWant[NACCT+1];
  long long sum = 0, n = 0, nHist = 0, nWantHist = 0;
  sqlite3_stmt *p;
  int i, s, rc, bad = 0;
  for(i=1; i<=NACCT; i++) aWant[i] = 1000;
  for(i=0; i<nThr; i++){
    for(s=0; s<nTxnPer; s++){
      if( !aThr[i].aDone[s] ) continue;
      aWant[aThr[i].aFrom[s]] -= aThr[i].aAmt[s];
      aWant[aThr[i].aTo[s]] += aThr[i].aAmt[s];
      nWantHist++;
    }
  }
  if( sumBal(db, &sum, &n) ){ fail(0, zWhen, db); return 1; }
  if( sum!=(long long)NACCT*1000 || n!=NACCT ){
    fprintf(stderr, "FAIL %s: total %lld over %lld rows\n", zWhen, sum, n);
    return 1;
  }
  rc = sqlite3_prepare_v2(db, "SELECT id, bal FROM acct ORDER BY id", -1, &p, 0);
  if( rc ){ fail(0, zWhen, db); return 1; }
  while( sqlite3_step(p)==SQLITE_ROW ){
    int id = sqlite3_column_int(p, 0);
    long long bal = sqlite3_column_int64(p, 1);
    if( id<1 || id>NACCT || bal!=aWant[id] ){
      if( bad++<5 ) fprintf(stderr, "FAIL %s: acct %d = %lld, the log says %lld\n",
                            zWhen, id, bal, id>=1 && id<=NACCT ? aWant[id] : -1);
    }
  }
  sqlite3_finalize(p);
  rc = sqlite3_prepare_v2(db, "SELECT id FROM hist", -1, &p, 0);
  if( rc ){ fail(0, zWhen, db); return 1; }
  while( sqlite3_step(p)==SQLITE_ROW ){
    long long hid = sqlite3_column_int64(p, 0);
    int ti = (int)(hid / HIST_BASE), sq = (int)(hid % HIST_BASE);
    nHist++;
    if( ti<0 || ti>=nThr || sq<0 || sq>=nTxnPer || !aThr[ti].aDone[sq] ){
      if( bad++<5 ) fprintf(stderr, "FAIL %s: hist row %lld was never committed\n",
                            zWhen, hid);
    }
  }
  sqlite3_finalize(p);
  if( nHist!=nWantHist ){
    fprintf(stderr, "FAIL %s: %lld hist rows, %lld transfers committed\n",
            zWhen, nHist, nWantHist);
    bad++;
  }
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &p, 0);
  if( rc==SQLITE_OK && sqlite3_step(p)==SQLITE_ROW
   && strcmp((const char*)sqlite3_column_text(p, 0), "ok") ){
    fprintf(stderr, "FAIL %s: integrity_check: %s\n", zWhen,
            (const char*)sqlite3_column_text(p, 0));
    bad++;
  }
  sqlite3_finalize(p);
  return bad;
}

static sqlite3 *openConn(void){
  sqlite3 *db = 0;
  if( sqlite3_open_v2(zDb, &db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE
                      |SQLITE_OPEN_PRIVATECACHE|SQLITE_OPEN_NOMUTEX, 0) ){
    fprintf(stderr, "FAIL open: %s\n", sqlite3_errmsg(db));
    exit(1);
  }
  sqlite3_busy_timeout(db, 10000);
  sqlite3_exec(db, "PRAGMA journal_mode=wal", 0, 0, 0);
  if( zPragmas[0] && sqlite3_exec(db, zPragmas, 0, 0, 0) ){
    fprintf(stderr, "FAIL pragmas: %s\n", sqlite3_errmsg(db));
    exit(1);
  }
  return db;
}

int main(int argc, char **argv){
  pthread_t aTid[MAXTHR];
  sqlite3 *db;
  long nCommit = 0, nRetry = 0, nRead = 0, nCkpt = 0;
  int i, bad;
  if( argc<5 ){
    fprintf(stderr, "usage: %s DB SEED NTHREADS NTXN [PRAGMAS]\n", argv[0]);
    return 2;
  }
  zDb = argv[1];
  seed0 = strtoull(argv[2], 0, 10);
  nThr = atoi(argv[3]);
  nTxnPer = atoi(argv[4]);
  if( argc>5 ) zPragmas = argv[5];
  if( nThr<1 || nThr>MAXTHR || nTxnPer<1 ){ fprintf(stderr, "bad args\n"); return 2; }
  if( getenv("MT_LOG") ) sqlite3_config(SQLITE_CONFIG_LOG, logCb, (void*)0);
  { char b[600];
    snprintf(b, sizeof(b), "%s", zDb); remove(b);
    snprintf(b, sizeof(b), "%s-wal", zDb); remove(b);
    snprintf(b, sizeof(b), "%s-shm", zDb); remove(b); }

  db = openConn();
  if( sqlite3_exec(db,
        "CREATE TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER);"
        "CREATE TABLE hist(id INTEGER PRIMARY KEY, a INT, b INT, amt INT);"
        "BEGIN;"
        "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<400)"
        "  INSERT INTO acct SELECT x, 1000 FROM c;"
        "COMMIT;", 0, 0, 0) ){
    fprintf(stderr, "FAIL setup: %s\n", sqlite3_errmsg(db));
    return 1;
  }

  for(i=0; i<nThr; i++){
    Thr *t = &aThr[i];
    t->id = i;
    t->rng = (seed0+1)*0x9E3779B97F4A7C15ull + (unsigned long long)(i+1)*0xBF58476D1CE4E5B9ull;
    t->aDone = (char*)calloc(nTxnPer, 1);
    t->aFrom = (int*)calloc(nTxnPer, sizeof(int));
    t->aTo = (int*)calloc(nTxnPer, sizeof(int));
    t->aAmt = (int*)calloc(nTxnPer, sizeof(int));
    t->db = openConn();
  }
  for(i=0; i<nThr; i++) pthread_create(&aTid[i], 0, thrMain, &aThr[i]);
  for(i=0; i<nThr; i++) pthread_join(aTid[i], 0);
  for(i=0; i<nThr; i++){
    if( aThr[i].zErr[0] ) fprintf(stderr, "FAIL %s\n", aThr[i].zErr);
    nCommit += aThr[i].nCommit; nRetry += aThr[i].nRetry;
    nRead += aThr[i].nReadCheck; nCkpt += aThr[i].nCkpt;
  }
  if( bFail ) return 1;

  bad = finalCheck(db, "final (setup connection)");
  for(i=0; i<nThr; i++) sqlite3_close(aThr[i].db);
  sqlite3_close(db);
  db = openConn();
  bad += finalCheck(db, "after reopen");
  sqlite3_close(db);
  if( bad ) return 1;
  printf("OK %d threads x %d transfers: %ld committed, %ld retries, %ld snapshot "
         "checks, %ld checkpoints\n", nThr, nTxnPer, nCommit, nRetry, nRead, nCkpt);
  return 0;
}
