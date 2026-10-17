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
**   MT_KILL_MS=300 MT_ITERS=10 mt_bf DB SEED NTHREADS NTXN ["PRAGMAS"]
**     crash mode (see crashMode): NTXN bounds each child's transfers per thread;
**     MT_SAVE=dir keeps the post-kill files, MT_CHECKONLY=iter re-runs the check
**   MT_POWER=all|subset|torn (+ crash mode): cut the power instead of a kill;
**     MT_FULL=1 (with PRAGMA synchronous=FULL) = acknowledged commits must survive
** Validate on the stock build first.
*/
#include "sqlite3.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NACCT      400
#define MAXTHR     16
#define HIST_BASE  1000000000LL

static const char *zDb;
static const char *zPragmas = "";
static int nThr, nTxnPer;
static unsigned long long seed0;
static volatile int bFail;
static int ackFd = -1;      /* crash mode: "thread seq" per acknowledged commit */
static int gIter = 0;       /* crash mode: iteration, part of every hist id */

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
      if( rc==SQLITE_OK ) rc = addHist(t->db,
                                       (long long)(gIter*MAXTHR + t->id)*HIST_BASE + seq,
                                       from, to, amt);
      if( rc==SQLITE_OK ) rc = execStep(t->db, "COMMIT");
      if( rc==SQLITE_OK ){
        t->aDone[seq] = 1;
        t->nCommit++;
        if( ackFd>=0 ){
          /* COMMIT returned: this transfer must survive a crash from now on. */
          char zAck[48];
          int nAck = snprintf(zAck, sizeof(zAck), "%d %d\n", t->id, seq);
          if( write(ackFd, zAck, nAck)!=nAck ){ fail(t, "ack write", 0); break; }
        }
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


/* -------------------------------------------------------------------------
** Power-cut mode (MT_POWER=all|subset|torn, with crash mode), 2026-10-17.
** bench/powerloss/plvfs.c's undo-log shim, made safe for threads: ONE undo
** stack per file PATH (an fsync on any handle makes the whole file durable),
** and every write/truncate/sync runs under one mutex, so the cut sees exactly
** what reached the "device".  At the first file event after MT_KILL_MS the
** unsynced writes are reverted -- all, a random subset, or torn at 512-byte
** sectors -- and the child _exit()s.  The parent then checks as for a kill,
** except that with synchronous!=FULL an acknowledged commit may be lost: what
** survives must then be, per thread, a prefix of what it committed.
*/
typedef struct PlUndo PlUndo;
struct PlUndo {
  sqlite3_int64 iOff, szBefore;
  int n, nOld, isTrunc;
  unsigned char *aOld;
  PlUndo *pNext;
};
typedef struct PlPath PlPath;
struct PlPath { char zName[512]; sqlite3_file *pAny; PlUndo *pUndo; PlPath *pNext; };
typedef struct PlFile PlFile;
struct PlFile { sqlite3_file base; sqlite3_file *pReal; PlPath *pPath; };

static sqlite3_vfs *plReal;
static sqlite3_vfs plVfs;
static PlPath *plPaths;
static pthread_mutex_t plMutex = PTHREAD_MUTEX_INITIALIZER;
static int plMode;                    /* 0 all, 1 subset, 2 torn */
static unsigned plRandState = 1;
static struct timespec plStart;
static long plCutMs;

static unsigned plRand(void){
  plRandState = plRandState*1103515245u + 12345u;
  return (plRandState>>16) & 0x7fff;
}
static long plElapsedMs(void){
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (t.tv_sec - plStart.tv_sec)*1000 + (t.tv_nsec - plStart.tv_nsec)/1000000;
}
/* With plMutex held: revert the unsynced writes, newest first, and die. */
static void plCutNow(void){
  PlPath *pp;
  for(pp=plPaths; pp; pp=pp->pNext){
    PlUndo *r;
    sqlite3_file *f = pp->pAny;
    if( f==0 ) continue;
    for(r=pp->pUndo; r; r=r->pNext){
      if( r->isTrunc ){
        if( r->nOld>0 ) f->pMethods->xWrite(f, r->aOld, r->nOld, r->iOff);
        continue;
      }
      if( plMode==1 && (plRand()&1) ) continue;
      if( plMode==2 ){
        int nSec = (r->n+511)/512, keep = (int)(plRand() % (unsigned)(nSec+1));
        int from = keep*512;
        if( from < r->nOld ) f->pMethods->xWrite(f, r->aOld+from, r->nOld-from, r->iOff+from);
        if( from < r->n && r->iOff + r->n > r->szBefore ){
          sqlite3_int64 cut = r->iOff+from > r->szBefore ? r->iOff+from : r->szBefore;
          f->pMethods->xTruncate(f, cut);
        }
        continue;
      }
      if( r->nOld>0 ) f->pMethods->xWrite(f, r->aOld, r->nOld, r->iOff);
      if( r->iOff + r->n > r->szBefore ) f->pMethods->xTruncate(f, r->szBefore);
    }
  }
  _exit(0);
}
static void plEvent(void){ if( plElapsedMs()>=plCutMs ) plCutNow(); }
static PlUndo *plSave(PlFile *p, sqlite3_int64 o, int n){
  sqlite3_int64 sz = 0;
  PlUndo *r = (PlUndo*)calloc(1, sizeof(*r));
  if( !r ) return 0;
  p->pReal->pMethods->xFileSize(p->pReal, &sz);
  r->iOff = o; r->n = n; r->szBefore = sz;
  if( o < sz ){
    r->nOld = (int)((o+n <= sz) ? n : sz-o);
    r->aOld = (unsigned char*)malloc(r->nOld);
    if( r->aOld==0 ){ free(r); return 0; }
    if( p->pReal->pMethods->xRead(p->pReal, r->aOld, r->nOld, o)!=SQLITE_OK ){
      memset(r->aOld, 0, r->nOld);
    }
  }
  r->pNext = p->pPath->pUndo; p->pPath->pUndo = r;
  return r;
}
static int plClose(sqlite3_file *f){
  PlFile *p = (PlFile*)f;
  int rc;
  pthread_mutex_lock(&plMutex);
  if( p->pPath && p->pPath->pAny==p->pReal ) p->pPath->pAny = 0;
  rc = p->pReal->pMethods->xClose(p->pReal);
  pthread_mutex_unlock(&plMutex);
  return rc;
}
static int plRead(sqlite3_file *f, void *z, int n, sqlite3_int64 o){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xRead(p->pReal, z, n, o);
}
static int plWrite(sqlite3_file *f, const void *z, int n, sqlite3_int64 o){
  PlFile *p = (PlFile*)f;
  int rc;
  if( p->pPath==0 ) return p->pReal->pMethods->xWrite(p->pReal, z, n, o);
  pthread_mutex_lock(&plMutex);
  p->pPath->pAny = p->pReal;
  plEvent();                          /* the cut may land BEFORE this write */
  if( plSave(p, o, n)==0 ){ pthread_mutex_unlock(&plMutex); return SQLITE_NOMEM; }
  rc = p->pReal->pMethods->xWrite(p->pReal, z, n, o);
  pthread_mutex_unlock(&plMutex);
  return rc;
}
static int plTruncate(sqlite3_file *f, sqlite3_int64 size){
  PlFile *p = (PlFile*)f;
  int rc;
  if( p->pPath==0 ) return p->pReal->pMethods->xTruncate(p->pReal, size);
  pthread_mutex_lock(&plMutex);
  p->pPath->pAny = p->pReal;
  plEvent();
  {
    sqlite3_int64 sz = 0;
    p->pReal->pMethods->xFileSize(p->pReal, &sz);
    if( size < sz ){
      PlUndo *r = (PlUndo*)calloc(1, sizeof(*r));
      if( r ){
        r->isTrunc = 1; r->iOff = size; r->szBefore = sz; r->nOld = (int)(sz-size);
        r->aOld = (unsigned char*)malloc(r->nOld);
        if( r->aOld ) p->pReal->pMethods->xRead(p->pReal, r->aOld, r->nOld, size);
        else r->nOld = 0;
        r->pNext = p->pPath->pUndo; p->pPath->pUndo = r;
      }
    }
  }
  rc = p->pReal->pMethods->xTruncate(p->pReal, size);
  pthread_mutex_unlock(&plMutex);
  return rc;
}
static int plSync(sqlite3_file *f, int flags){
  PlFile *p = (PlFile*)f;
  int rc;
  if( p->pPath==0 ) return p->pReal->pMethods->xSync(p->pReal, flags);
  pthread_mutex_lock(&plMutex);
  p->pPath->pAny = p->pReal;
  plEvent();                          /* a cut here: this sync never happened */
  rc = p->pReal->pMethods->xSync(p->pReal, flags);
  if( rc==SQLITE_OK ){
    PlUndo *r = p->pPath->pUndo;      /* the whole FILE is durable now */
    while( r ){ PlUndo *nx = r->pNext; free(r->aOld); free(r); r = nx; }
    p->pPath->pUndo = 0;
  }
  pthread_mutex_unlock(&plMutex);
  return rc;
}
static int plFileSize(sqlite3_file *f, sqlite3_int64 *s){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xFileSize(p->pReal, s); }
static int plLock(sqlite3_file *f, int l){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xLock(p->pReal, l); }
static int plUnlock(sqlite3_file *f, int l){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xUnlock(p->pReal, l); }
static int plCheck(sqlite3_file *f, int *r){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xCheckReservedLock(p->pReal, r); }
static int plFcntl(sqlite3_file *f, int op, void *a){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xFileControl(p->pReal, op, a); }
static int plSector(sqlite3_file *f){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xSectorSize(p->pReal); }
static int plDevChar(sqlite3_file *f){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xDeviceCharacteristics(p->pReal); }
static int plShmMap(sqlite3_file *f, int i, int sz, int w, void volatile **pp){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xShmMap(p->pReal, i, sz, w, pp); }
static int plShmLock(sqlite3_file *f, int o, int n, int fl){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xShmLock(p->pReal, o, n, fl); }
static void plShmBarrier(sqlite3_file *f){ PlFile *p=(PlFile*)f; p->pReal->pMethods->xShmBarrier(p->pReal); }
static int plShmUnmap(sqlite3_file *f, int d){ PlFile *p=(PlFile*)f; return p->pReal->pMethods->xShmUnmap(p->pReal, d); }
static const sqlite3_io_methods plMethods = {
  2, plClose, plRead, plWrite, plTruncate, plSync, plFileSize, plLock, plUnlock,
  plCheck, plFcntl, plSector, plDevChar, plShmMap, plShmLock, plShmBarrier,
  plShmUnmap, 0, 0
};
static int plOpen(sqlite3_vfs *v, const char *zName, sqlite3_file *f, int flags, int *pOut){
  PlFile *p = (PlFile*)f;
  int rc;
  (void)v;
  memset(p, 0, sizeof(*p));
  p->pReal = (sqlite3_file*)&p[1];
  rc = plReal->xOpen(plReal, zName, p->pReal, flags, pOut);
  if( rc!=SQLITE_OK ){ p->base.pMethods = 0; return rc; }
  if( zName && (flags & (SQLITE_OPEN_MAIN_DB|SQLITE_OPEN_WAL))!=0 ){
    PlPath *pp;
    pthread_mutex_lock(&plMutex);
    for(pp=plPaths; pp && strcmp(pp->zName, zName); pp=pp->pNext){}
    if( pp==0 && (pp = (PlPath*)calloc(1, sizeof(*pp)))!=0 ){
      snprintf(pp->zName, sizeof(pp->zName), "%s", zName);
      pp->pNext = plPaths; plPaths = pp;
    }
    p->pPath = pp;
    if( pp ) pp->pAny = p->pReal;
    pthread_mutex_unlock(&plMutex);
  }
  p->base.pMethods = &plMethods;
  return SQLITE_OK;
}
static void plInstall(const char *zMode, long cutMs, unsigned seed){
  plMode = !strcmp(zMode, "subset") ? 1 : !strcmp(zMode, "torn") ? 2 : 0;
  plCutMs = cutMs;
  plRandState = seed ? seed : 1;
  clock_gettime(CLOCK_MONOTONIC, &plStart);
  plReal = sqlite3_vfs_find(0);
  plVfs = *plReal;
  plVfs.zName = "mtpower";
  plVfs.szOsFile = (int)sizeof(PlFile) + plReal->szOsFile;
  plVfs.xOpen = plOpen;
  sqlite3_vfs_register(&plVfs, 1);
}
/*
** Crash mode (MT_KILL_MS=max milliseconds, MT_ITERS=n): each iteration forks a
** child that runs the threaded workload with unbounded transfers, SIGKILLs it
** at a random moment -- mid-commit, mid-checkpoint, mid-catch-up -- and then
** checks the recovered file on a fresh connection:
**   - integrity_check, and the invariant total;
**   - ATOMICITY: every balance equals 1000 plus what the surviving hist rows
**     moved into it, minus what they moved out (a transfer and its hist row
**     commit together or not at all);
**   - DURABILITY of acknowledged commits: every (thread, seq) the child wrote
**     to the ack file after COMMIT returned has its hist row.  A process kill
**     loses no OS buffers, so with the default durable mode (records reach the
**     OS before COMMIT returns) nothing acknowledged may be missing.
** Iterations accumulate on the same file, so each one also recovers from the
** previous crash and keeps working.
*/
static int crashCheck(const char *zAck){
  sqlite3 *db = openConn();
  sqlite3_stmt *p;
  long long sum = 0, n = 0;
  int bad = 0, rc;
  FILE *f;
  if( sumBal(db, &sum, &n) ){
    fprintf(stderr, "FAIL iter %d: sum: %s\n", gIter, sqlite3_errmsg(db));
    sqlite3_close(db);
    return 1;
  }
  if( sum!=(long long)NACCT*1000 || n!=NACCT ){
    fprintf(stderr, "FAIL iter %d: total %lld over %lld rows\n", gIter, sum, n);
    bad++;
  }
  rc = sqlite3_prepare_v2(db,
      "SELECT acct.id, acct.bal, 1000"
      " + coalesce((SELECT sum(amt) FROM hist WHERE b=acct.id),0)"
      " - coalesce((SELECT sum(amt) FROM hist WHERE a=acct.id),0)"
      " FROM acct", -1, &p, 0);
  if( rc==SQLITE_OK ){
    while( sqlite3_step(p)==SQLITE_ROW ){
      if( sqlite3_column_int64(p, 1)!=sqlite3_column_int64(p, 2) ){
        if( bad++<5 ){
          fprintf(stderr, "FAIL iter %d: acct %d = %lld, hist says %lld (atomicity)\n",
                  gIter, sqlite3_column_int(p, 0), sqlite3_column_int64(p, 1),
                  sqlite3_column_int64(p, 2));
        }
      }
    }
    sqlite3_finalize(p);
  }else{
    bad++;
  }
  if( getenv("MT_POWER") && !getenv("MT_FULL") ){
    /* synchronous!=FULL: acknowledged commits may be lost to a power cut, but
    ** what survives is a prefix of the log, so per thread a prefix 0..k of
    ** this iteration's transfers. */
    sqlite3_stmt *q;
    int ti;
    sqlite3_prepare_v2(db, "SELECT count(*), coalesce(max(id),-1) FROM hist WHERE id>=? AND id<?",
                       -1, &q, 0);
    for(ti=0; ti<nThr; ti++){
      long long lo = (long long)(gIter*MAXTHR + ti)*HIST_BASE;
      sqlite3_bind_int64(q, 1, lo);
      sqlite3_bind_int64(q, 2, lo + HIST_BASE);
      if( sqlite3_step(q)==SQLITE_ROW ){
        long long nr = sqlite3_column_int64(q, 0), mx = sqlite3_column_int64(q, 1);
        if( nr>0 && mx-lo+1!=nr ){
          if( bad++<5 ) fprintf(stderr, "FAIL iter %d: thread %d kept %lld transfers "
                                "but its last is seq %lld (not a prefix)\n",
                                gIter, ti, nr, mx-lo);
        }
      }
      sqlite3_reset(q);
    }
    sqlite3_finalize(q);
  }else
  if( (f = fopen(zAck, "r"))!=0 ){
    int ti, sq;
    sqlite3_stmt *q;
    sqlite3_prepare_v2(db, "SELECT 1 FROM hist WHERE id=?", -1, &q, 0);
    while( fscanf(f, "%d %d", &ti, &sq)==2 ){
      sqlite3_bind_int64(q, 1, (long long)(gIter*MAXTHR + ti)*HIST_BASE + sq);
      if( sqlite3_step(q)!=SQLITE_ROW ){
        if( bad++<5 ) fprintf(stderr, "FAIL iter %d: acknowledged transfer "
                              "thread %d seq %d is gone (durability)\n", gIter, ti, sq);
      }
      sqlite3_reset(q);
    }
    sqlite3_finalize(q);
    fclose(f);
  }
  rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &p, 0);
  if( rc==SQLITE_OK && sqlite3_step(p)==SQLITE_ROW
   && strcmp((const char*)sqlite3_column_text(p, 0), "ok") ){
    fprintf(stderr, "FAIL iter %d: integrity_check: %s\n", gIter,
            (const char*)sqlite3_column_text(p, 0));
    bad++;
  }
  sqlite3_finalize(p);
  sqlite3_close(db);
  return bad;
}

static int crashMode(int killMs, int nIter){
  char zAck[600];
  long nAcked = 0;
  int i;
  snprintf(zAck, sizeof(zAck), "%s-ack", zDb);
  for(gIter=0; gIter<nIter; gIter++){
    pid_t pid;
    int status;
    unsigned long long r = (seed0+7)*0x9E3779B97F4A7C15ull + (unsigned)gIter*7919u;
    long ms = killMs/4 + (long)((r>>17) % (unsigned long long)(killMs - killMs/4 + 1));
    remove(zAck);
    pid = fork();
    if( pid==0 ){
      pthread_t aTid[MAXTHR];
      if( getenv("MT_POWER") ) plInstall(getenv("MT_POWER"), ms, (unsigned)(r>>9));
      ackFd = open(zAck, O_WRONLY|O_CREAT|O_APPEND, 0644);
      for(i=0; i<nThr; i++){
        Thr *t = &aThr[i];
        t->id = i;
        t->rng = (seed0+1+gIter)*0x9E3779B97F4A7C15ull
               + (unsigned long long)(i+1)*0xBF58476D1CE4E5B9ull;
        t->db = openConn();
      }
      for(i=0; i<nThr; i++) pthread_create(&aTid[i], 0, thrMain, &aThr[i]);
      for(i=0; i<nThr; i++) pthread_join(aTid[i], 0);
      for(i=0; i<nThr; i++) if( aThr[i].zErr[0] ) fprintf(stderr, "FAIL %s\n", aThr[i].zErr);
      _exit(bFail ? 3 : 0);
    }
    if( getenv("MT_POWER") ){
      /* The child cuts its own power at its first file event after ms; the
      ** kill below only bounds a child that stopped doing I/O. */
      long waited = 0;
      while( waitpid(pid, &status, WNOHANG)==0 ){
        struct timespec ts = {0, 20000000L};
        nanosleep(&ts, 0);
        waited += 20;
        if( waited > ms + 30000 ){ kill(pid, SIGKILL); waitpid(pid, &status, 0); break; }
      }
    }else{
      struct timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (ms%1000)*1000000L;
      nanosleep(&ts, 0);
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
    if( WIFEXITED(status) && WEXITSTATUS(status)!=0 ){
      fprintf(stderr, "FAIL iter %d: the child failed before the kill\n", gIter);
      return 1;
    }
    { FILE *f = fopen(zAck, "r"); int c;
      if( f ){ while( (c=fgetc(f))!=EOF ) if( c=='\n' ) nAcked++; fclose(f); } }
    if( getenv("MT_SAVE") ){
      /* Keep the files exactly as the kill left them, to replay the check. */
      char zCmd[2000];
      snprintf(zCmd, sizeof(zCmd), "mkdir -p '%s' && cp '%s' '%s-wal' '%s-shm' '%s' '%s'/ 2>/dev/null",
               getenv("MT_SAVE"), zDb, zDb, zDb, zAck, getenv("MT_SAVE"));
      if( system(zCmd) ){ /* best effort */ }
    }
    if( crashCheck(zAck) ) return 1;
  }
  printf("OK crash mode: %d threads, %d kills, %ld acknowledged transfers checked\n",
         nThr, nIter, nAcked);
  return 0;
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
  if( !getenv("MT_CHECKONLY") ){ char b[600];
    snprintf(b, sizeof(b), "%s", zDb); remove(b);
    snprintf(b, sizeof(b), "%s-wal", zDb); remove(b);
    snprintf(b, sizeof(b), "%s-shm", zDb); remove(b); }

  db = getenv("MT_CHECKONLY") ? 0 : openConn();
  if( !getenv("MT_CHECKONLY") && sqlite3_exec(db,
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
  }
  if( getenv("MT_CHECKONLY") ){
    /* Replay crashCheck on saved files: DB must not be recreated (see above).
    ** The connection opened above is closed first, so the check's own
    ** connection runs WAL recovery, exactly as in crash mode. */
    char zAck[600];
    sqlite3_close(db);
    gIter = atoi(getenv("MT_CHECKONLY"));
    snprintf(zAck, sizeof(zAck), "%s-ack", zDb);
    return crashCheck(zAck) ? 1 : (printf("OK check\n"), 0);
  }
  if( getenv("MT_KILL_MS") ){
    int killMs = atoi(getenv("MT_KILL_MS"));
    int nIter = getenv("MT_ITERS") ? atoi(getenv("MT_ITERS")) : 10;
    if( sqlite3_close(db)!=SQLITE_OK ) fprintf(stderr, "setup close: %s\n", sqlite3_errmsg(db));
    if( killMs<4 ) killMs = 4;
    return crashMode(killMs, nIter);
  }
  for(i=0; i<nThr; i++) aThr[i].db = openConn();
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
