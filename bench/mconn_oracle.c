/*
** mconn_oracle.c -- multi-connection snapshot oracle: ONE writer, K readers.
**
** The concurrency model the owner chose (2026-10-12) is SQLite's own: one
** writer connection, N reader connections, every connection with its OWN cache
** (BF ring or stock page cache).  The differential oracles drive a single
** connection, so they cannot see cross-connection bugs -- and in this fork the
** committed state is not only in pages: buffered rows live in the writer's
** mini-pages and in WAL RECORD frames.  A reader must see exactly the snapshot
** that was committed when its read transaction started, no more, no less.
**
** All connections live in one process and one thread (separate pagers, no
** shared-cache mode), interleaved by a seeded RNG, so a failure replays
** exactly.  The model is in-process: an array of committed values, the
** writer's working copy inside its transaction, and each reader's snapshot
** copied at its FIRST read inside a transaction (WAL fixes the snapshot there,
** not at BEGIN).  Every read is checked against the model; the first mismatch
** prints and exits 1.
**
** Build against either amalgamation -- the oracle must pass on STOCK first,
** that is what validates the oracle:
**   cc -O1 -g -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
**      -o build/mconn_buf bench/mconn_oracle.c build/sqlite3.c -lm -lpthread
**   cc -O1 -g -DSQLITE_OMIT_BF_CACHE -Ibuild -Isrc \
**      -o build/mconn_stock bench/mconn_oracle.c build/sqlite3.c -lm -lpthread
** Usage:
**   mconn_buf DB SEED NOPS NREADERS ["PRAGMAS"] ["READER PRAGMAS"]
** PRAGMAS go to EVERY connection after journal_mode=wal; READER PRAGMAS, when
** given, go to the readers only, after PRAGMAS (e.g. a smaller ring).
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned long long u64;

#define NKEYS     3000
#define MAXREAD   8
#define MAXVAL    300

static unsigned long long rngState;
static unsigned rnd(void){
  rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17;
  return (unsigned)(rngState >> 11);
}
static int rndn(int n){ return (int)(rnd() % (unsigned)n); }

static char *aModel[NKEYS+1];    /* committed state; NULL = absent */
static char *aWork[NKEYS+1];     /* writer's working copy while in a txn */

typedef struct Conn Conn;
struct Conn {
  sqlite3 *db;
  int inTxn;                     /* explicit BEGIN issued */
  int hasSnap;                   /* first read in the txn happened */
  long iSnapOp;                  /* op at which the snapshot was taken */
  char *aSnap[NKEYS+1];          /* snapshot (readers), shared pointers */
};
static long iOpCur;
static Conn W;
/* Failure forensics: per key, the last few writer events. */
#define NHIST 8
typedef struct Ev { long op; char kind; } Ev;
static Ev aHist[NKEYS+1][NHIST];
static int nHist[NKEYS+1];
static void note(int k, char kind){
  Ev *e = &aHist[k][nHist[k]++ % NHIST];
  e->op = iOpCur; e->kind = kind;
}

static Conn R[MAXREAD];
static int nReader;
static long iOp;
static const char *zDbPath;

static void die(const char *zMsg){
  fprintf(stderr, "FAIL op=%ld seed=%llu: %s\n", iOp, rngState, zMsg);
  exit(1);
}
static void execOk(sqlite3 *db, const char *zSql){
  char *zErr = 0;
  if( sqlite3_exec(db, zSql, 0, 0, &zErr)!=SQLITE_OK ){
    char buf[512];
    snprintf(buf, sizeof(buf), "exec \"%s\": %s", zSql, zErr ? zErr : "?");
    die(buf);
  }
}

/* Values are refcount-free: every version is a fresh malloc and nothing is
** freed (bounded by NOPS), so snapshots can share pointers safely. */
static char *mkVal(void){
  int n = 1 + rndn(MAXVAL), i;
  char *z = malloc(n+1);
  for(i=0; i<n; i++) z[i] = 'a' + rndn(26);
  z[n] = 0;
  return z;
}

static char **viewOf(Conn *c){
  if( c==&W ) return W.inTxn ? aWork : aModel;
  if( c->inTxn ){
    if( !c->hasSnap ){ memcpy(c->aSnap, aModel, sizeof(aModel)); c->hasSnap = 1;
                       c->iSnapOp = iOpCur; }
    return c->aSnap;
  }
  return aModel;                 /* autocommit read: the latest commit */
}

static const char *connName(Conn *c){
  static char buf[16];
  if( c==&W ) return "writer";
  snprintf(buf, sizeof(buf), "reader%d", (int)(c-R));
  return buf;
}

static void checkPoint(Conn *c, int k){
  char **v = viewOf(c);
  sqlite3_stmt *p;
  const char *zGot = 0;
  int rc;
  sqlite3_prepare_v2(c->db, "SELECT v FROM t WHERE k=?", -1, &p, 0);
  sqlite3_bind_int(p, 1, k);
  rc = sqlite3_step(p);
  if( rc==SQLITE_ROW ) zGot = (const char*)sqlite3_column_text(p, 0);
  else if( rc!=SQLITE_DONE ) die(sqlite3_errmsg(c->db));
  if( (zGot==0) != (v[k]==0) || (zGot && strcmp(zGot, v[k])!=0) ){
    char buf[256];
    snprintf(buf, sizeof(buf), "%s point k=%d expected %s got %s",
             connName(c), k, v[k] ? "a row" : "ABSENT", zGot ? "a row" : "ABSENT");
    if( zGot && v[k] ){
      snprintf(buf, sizeof(buf), "%s point k=%d value differs (len %d vs %d)",
               connName(c), k, (int)strlen(v[k]), (int)strlen(zGot));
    }
    sqlite3_finalize(p);
    die(buf);
  }
  sqlite3_finalize(p);
}

static void checkRange(Conn *c, int lo, int hi, int bDesc){
  char **v = viewOf(c);
  sqlite3_stmt *p;
  int k = bDesc ? hi : lo, rc;
  sqlite3_prepare_v2(c->db, bDesc
      ? "SELECT k, v FROM t WHERE k BETWEEN ? AND ? ORDER BY k DESC"
      : "SELECT k, v FROM t WHERE k BETWEEN ? AND ? ORDER BY k", -1, &p, 0);
  sqlite3_bind_int(p, 1, lo);
  sqlite3_bind_int(p, 2, hi);
  while( (rc = sqlite3_step(p))==SQLITE_ROW ){
    int kg = sqlite3_column_int(p, 0);
    const char *zg = (const char*)sqlite3_column_text(p, 1);
    /* advance the model cursor to the next present key */
    while( bDesc ? (k>=lo && v[k]==0) : (k<=hi && v[k]==0) ) k += bDesc ? -1 : 1;
    if( (bDesc ? k<lo : k>hi) || kg!=k || strcmp(zg, v[k])!=0 ){
      char buf[256];
      snprintf(buf, sizeof(buf), "%s range [%d,%d]%s: got k=%d, model k=%d%s",
               connName(c), lo, hi, bDesc ? " DESC" : "", kg, k,
               (kg==k) ? " (value differs)" : "");
      { int kk, j;
        fprintf(stderr, "%s inTxn=%d snapAt=%ld; got len %d model len %d\n",
                connName(c), c->inTxn, c->hasSnap ? c->iSnapOp : -1L,
                (int)strlen(zg), (k>=1&&k<=NKEYS&&v[k]) ? (int)strlen(v[k]) : -1);
        for(kk=0; kk<2; kk++){
          int key = kk ? kg : k, n2;
          if( key<1 || key>NKEYS ) continue;
          n2 = nHist[key]<NHIST ? nHist[key] : NHIST;
          fprintf(stderr, "  k=%d history:", key);
          for(j=0; j<n2; j++){
            Ev *e = &aHist[key][(nHist[key]-n2+j)%NHIST];
            fprintf(stderr, " %ld%c", e->op, e->kind);
          }
          fprintf(stderr, "\n");
        }
        /* which committed version does the db value match? */
      }
      sqlite3_finalize(p);
      die(buf);
    }
    k += bDesc ? -1 : 1;
  }
  if( rc!=SQLITE_DONE ) die(sqlite3_errmsg(c->db));
  while( bDesc ? (k>=lo && v[k]==0) : (k<=hi && v[k]==0) ) k += bDesc ? -1 : 1;
  if( bDesc ? k>=lo : k<=hi ){
    char buf[256];
    snprintf(buf, sizeof(buf), "%s range [%d,%d]%s: missing model k=%d",
             connName(c), lo, hi, bDesc ? " DESC" : "", k);
    sqlite3_finalize(p);
    die(buf);
  }
  sqlite3_finalize(p);
}

static void dumpDiff(Conn *c){
  char **v = viewOf(c);
  static char seen[NKEYS+1];
  sqlite3_stmt *p;
  int k, nOut = 0;
  memset(seen, 0, sizeof(seen));
  sqlite3_prepare_v2(c->db, "SELECT k FROM t", -1, &p, 0);
  while( sqlite3_step(p)==SQLITE_ROW ){
    k = sqlite3_column_int(p, 0);
    if( k>=1 && k<=NKEYS ){
      if( seen[k] && nOut<6 ){
        int i, n = nHist[k]<NHIST ? nHist[k] : NHIST;
        fprintf(stderr, "  DUPLICATE k=%d  history:", k);
        for(i=0; i<n; i++){
          Ev *e = &aHist[k][(nHist[k]-n+i)%NHIST];
          fprintf(stderr, " %ld%c", e->op, e->kind);
        }
        fprintf(stderr, "\n");
        nOut++;
      }
      seen[k] = 1;
    }
  }
  sqlite3_finalize(p);
  fprintf(stderr, "%s inTxn=%d hasSnap=%d writer inTxn=%d\n", connName(c),
          c->inTxn, c->hasSnap, W.inTxn);
  for(k=1; k<=NKEYS && nOut<6; k++){
    if( seen[k]!=(v[k]!=0) ){
      int i, n = nHist[k]<NHIST ? nHist[k] : NHIST;
      fprintf(stderr, "  k=%d model=%s db=%s  history:", k, v[k]?"present":"absent",
              seen[k]?"present":"absent");
      for(i=0; i<n; i++){
        Ev *e = &aHist[k][(nHist[k]-n+i)%NHIST];
        fprintf(stderr, " %ld%c", e->op, e->kind);
      }
      fprintf(stderr, "\n");
      nOut++;
    }
  }
}

static void checkCount(Conn *c){
  char **v = viewOf(c);
  sqlite3_stmt *p;
  int n = 0, k;
  for(k=1; k<=NKEYS; k++) if( v[k] ) n++;
  sqlite3_prepare_v2(c->db, "SELECT count(*) FROM t", -1, &p, 0);
  if( sqlite3_step(p)!=SQLITE_ROW ) die(sqlite3_errmsg(c->db));
  if( sqlite3_column_int(p, 0)!=n ){
    dumpDiff(c);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s count: model %d got %d", connName(c), n,
             sqlite3_column_int(p, 0));
    sqlite3_finalize(p);
    die(buf);
  }
  sqlite3_finalize(p);
}

static void anyRead(Conn *c){
  int r = rndn(10);
  if( r<6 ) checkPoint(c, 1 + rndn(NKEYS));
  else if( r<9 ){
    int lo = 1 + rndn(NKEYS), hi = lo + rndn(200);
    if( hi>NKEYS ) hi = NKEYS;
    checkRange(c, lo, hi, rndn(3)==0);
  }else checkCount(c);
}

static void writeOne(void){
  int k = 1 + rndn(NKEYS);
  char **v = W.inTxn ? aWork : aModel;
  sqlite3_stmt *p;
  if( v[k]==0 ){
    char *z = mkVal();
    sqlite3_prepare_v2(W.db, "INSERT INTO t(k,v) VALUES(?,?)", -1, &p, 0);
    sqlite3_bind_int(p, 1, k);
    sqlite3_bind_text(p, 2, z, -1, SQLITE_STATIC);
    if( sqlite3_step(p)!=SQLITE_DONE ) die(sqlite3_errmsg(W.db));
    v[k] = z; note(k, W.inTxn ? 'i' : 'I');
  }else if( rndn(4)==0 ){
    sqlite3_prepare_v2(W.db, "DELETE FROM t WHERE k=?", -1, &p, 0);
    sqlite3_bind_int(p, 1, k);
    if( sqlite3_step(p)!=SQLITE_DONE ) die(sqlite3_errmsg(W.db));
    v[k] = 0; note(k, W.inTxn ? 'd' : 'D');
  }else{
    char *z = mkVal();
    sqlite3_prepare_v2(W.db, "UPDATE t SET v=? WHERE k=?", -1, &p, 0);
    sqlite3_bind_text(p, 1, z, -1, SQLITE_STATIC);
    sqlite3_bind_int(p, 2, k);
    if( sqlite3_step(p)!=SQLITE_DONE ) die(sqlite3_errmsg(W.db));
    v[k] = z; note(k, W.inTxn ? 'u' : 'U');
  }
  sqlite3_finalize(p);
}

static void openConn(Conn *c, const char *zPragmas){
  if( sqlite3_open_v2(zDbPath, &c->db,
        SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_PRIVATECACHE, 0) ){
    die("open");
  }
  execOk(c->db, "PRAGMA journal_mode=wal;");
  if( zPragmas && zPragmas[0] ) execOk(c->db, zPragmas);
}

int main(int argc, char **argv){
  long nOps;
  int i, k;
  const char *zPragmas;
  const char *zReaderPragmas;
  if( argc<5 ){
    fprintf(stderr, "usage: %s DB SEED NOPS NREADERS [PRAGMAS]\n", argv[0]);
    return 2;
  }
  zDbPath = argv[1];
  rngState = strtoull(argv[2], 0, 10)*0x9E3779B97F4A7C15ull + 1;
  nOps = atol(argv[3]);
  nReader = atoi(argv[4]);
  if( nReader<1 || nReader>MAXREAD ) nReader = 2;
  zPragmas = argc>5 ? argv[5] : "";
  zReaderPragmas = argc>6 ? argv[6] : "";
  { char b[600];
    snprintf(b, sizeof(b), "%s", zDbPath); remove(b);
    snprintf(b, sizeof(b), "%s-wal", zDbPath); remove(b);
    snprintf(b, sizeof(b), "%s-shm", zDbPath); remove(b); }

  openConn(&W, zPragmas);
  execOk(W.db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT)");
  execOk(W.db, "BEGIN");
  W.inTxn = 1;
  memcpy(aWork, aModel, sizeof(aModel));
  for(k=1; k<=NKEYS; k+=2){
    int save = k; (void)save;
    { sqlite3_stmt *p; char *z = mkVal();
      sqlite3_prepare_v2(W.db, "INSERT INTO t(k,v) VALUES(?,?)", -1, &p, 0);
      sqlite3_bind_int(p, 1, k);
      sqlite3_bind_text(p, 2, z, -1, SQLITE_STATIC);
      if( sqlite3_step(p)!=SQLITE_DONE ) die(sqlite3_errmsg(W.db));
      sqlite3_finalize(p);
      aWork[k] = z; }
  }
  execOk(W.db, "COMMIT");
  memcpy(aModel, aWork, sizeof(aModel));
  W.inTxn = 0;
  for(i=0; i<nReader; i++){
    openConn(&R[i], zPragmas);
    if( zReaderPragmas[0] ) execOk(R[i].db, zReaderPragmas);
  }

  for(iOp=0; iOp<nOps; iOp++){
    int r = rndn(100);
    iOpCur = iOp;
    if( r<40 ){                                   /* ---- writer ---- */
      if( !W.inTxn ){
        int w = rndn(10);
        if( w<5 ){ if(getenv("MCONN_TRACE")) fprintf(stderr,"%ld BEGIN\n",iOp);
                   execOk(W.db, "BEGIN"); W.inTxn = 1;
                   memcpy(aWork, aModel, sizeof(aModel)); }
        else if( w<9 ) writeOne();                /* autocommit write */
        else if( w==9 && rndn(4)==0 ){
          const char *zCk = rndn(3) ? "PRAGMA wal_checkpoint(PASSIVE)"
                                    : "PRAGMA wal_checkpoint(RESTART)";
          if(getenv("MCONN_TRACE")) fprintf(stderr,"%ld %s\n",iOp,zCk);
          execOk(W.db, zCk);
        }else anyRead(&W);
      }else{
        int w = rndn(20);
        if( w<12 ) writeOne();
        else if( w<16 ) anyRead(&W);
        else if( w<19 ){ if(getenv("MCONN_TRACE")) fprintf(stderr,"%ld COMMIT\n",iOp);
                         execOk(W.db, "COMMIT"); W.inTxn = 0;
                         memcpy(aModel, aWork, sizeof(aModel)); }
        else { if(getenv("MCONN_TRACE")) fprintf(stderr,"%ld ROLLBACK\n",iOp);
               execOk(W.db, "ROLLBACK"); W.inTxn = 0; }
      }
    }else{                                        /* ---- a reader ---- */
      Conn *c = &R[rndn(nReader)];
      int w = rndn(20);
      if( !c->inTxn && w<3 ){ execOk(c->db, "BEGIN"); c->inTxn = 1; c->hasSnap = 0;
        if(getenv("MCONN_TRACE")) fprintf(stderr,"%ld %s BEGIN\n",iOp,connName(c)); }
      else if( c->inTxn && w<5 ){ execOk(c->db, "COMMIT"); c->inTxn = 0; c->hasSnap = 0;
        if(getenv("MCONN_TRACE")) fprintf(stderr,"%ld %s END\n",iOp,connName(c)); }
      else anyRead(c);
    }
  }
  /* Final: every connection, fresh snapshot, full check. */
  if( W.inTxn ){ execOk(W.db, "COMMIT"); memcpy(aModel, aWork, sizeof(aModel)); W.inTxn = 0; }
  for(i=0; i<nReader; i++){
    if( R[i].inTxn ){ execOk(R[i].db, "COMMIT"); R[i].inTxn = 0; }
    checkRange(&R[i], 1, NKEYS, 0);
  }
  checkRange(&W, 1, NKEYS, 0);
  /* A replay that drops a record has lost a committed row even if no read
  ** happened to touch it: fail on it explicitly (BF builds only). */
  { u64 nDrop = 0, nRehyd = 0;
    for(i=0; i<=nReader; i++){
      sqlite3 *db = i<nReader ? R[i].db : W.db;
      sqlite3_stmt *p;
      if( sqlite3_prepare_v2(db, "PRAGMA bf_cache_stats", -1, &p, 0)!=SQLITE_OK ) break;
      while( sqlite3_step(p)==SQLITE_ROW ){
        const char *zName = (const char*)sqlite3_column_text(p, 0);
        if( zName && strcmp(zName, "replay_dropped")==0 ) nDrop += sqlite3_column_int64(p, 1);
        if( zName && strcmp(zName, "snapshot_rehydrate")==0 ) nRehyd += sqlite3_column_int64(p, 1);
      }
      sqlite3_finalize(p);
    }
    if( nDrop ){
      char buf[96];
      snprintf(buf, sizeof(buf), "replay_dropped=%llu: a replay lost committed records",
               (unsigned long long)nDrop);
      die(buf);
    }
    printf("OK %ld ops, %d readers, %llu snapshot rebuilds\n", nOps, nReader,
           (unsigned long long)nRehyd);
  }
  for(i=0; i<nReader; i++) sqlite3_close(R[i].db);
  sqlite3_close(W.db);
  return 0;
}
