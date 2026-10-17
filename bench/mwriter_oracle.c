/*
** mwriter_oracle.c -- multi-WRITER serialisability oracle for BEGIN CONCURRENT.
**
** K connections on one file (separate pagers, private caches), interleaved by a
** seeded RNG in ONE thread, each running BEGIN CONCURRENT transactions of a few
** reads and writes.  An in-process model checks two things:
**
**   1. Every read inside a transaction returns the transaction's own view: its
**      snapshot (taken at its first statement, as WAL does) plus its own writes.
**
**   2. Optimistic concurrency is serialisable in COMMIT order iff a WRITING
**      transaction that commits read nothing that another transaction changed
**      after its snapshot (a read-only one serialises at its snapshot).  So at each COMMIT the oracle compares everything the
**      transaction observed -- point keys, including absent ones, and whole
**      ranges -- with the committed state at that moment:
**        - COMMIT succeeded but something it read had changed -> FAIL (a lost
**          update / write skew the engine let through);
**        - COMMIT refused (SQLITE_BUSY_SNAPSHOT) though nothing it read had
**          changed -> a FALSE conflict: correct, but counted.  The false-conflict
**          rate is the metric page-level validation is expected to lose on.
**
** Values are unique per write ("c<conn>.t<txn>.o<op>"), so equal strings mean
** the same write.  Every statement's own row access counts as a read (UPDATE and
** DELETE find the row, INSERT probes for it): that is what the engine reads.
**
** Build against the merged tree (BEGIN CONCURRENT compiled in):
**   cc -O1 -g -DSQLITE_OMIT_BF_CACHE -Ibuild -Isrc \
**      -o build/mw_stock bench/mwriter_oracle.c build/sqlite3.c -lm -lpthread
**   cc -O1 -g -DSQLITE_BF_INSERT_BUFFERING -Ibuild -Isrc \
**      -o build/mw_bf bench/mwriter_oracle.c build/sqlite3.c -lm -lpthread
** Usage:
**   mw_bf DB SEED NOPS NCONN ["PRAGMAS"]
** The stock build must pass first: that validates the oracle.
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NKEYS    2000
#define MAXCONN  8
#define MAXOBS   256          /* observations per transaction */
#define MAXWR    64

static unsigned long long rngState;
static unsigned rnd(void){
  rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17;
  return (unsigned)(rngState >> 11);
}
static int rndn(int n){ return (int)(rnd() % (unsigned)n); }

static char *aCommitted[NKEYS+1];  /* the model: committed state, NULL = absent */
static const char *zDbPath;
static long iOp;

typedef struct Obs { int lo, hi; } Obs;      /* observed key range (point: lo==hi) */
typedef struct Wr { int k; char *v; } Wr;     /* v==NULL: delete */

typedef struct Conn {
  sqlite3 *db;
  int inTxn, hasSnap, nLeft, iTxn;
  char *aSnap[NKEYS+1];            /* committed state at the snapshot */
  Obs aObs[MAXOBS]; int nObs, bObsOverflow;
  Wr aWr[MAXWR]; int nWr;
} Conn;
static Conn C[MAXCONN];
static int nConn;

static long nCommit, nTrueConflict, nFalseConflict, nTxn;
static int bTrace;
static int bCheckSnap;
static int bCheckCommit;
static int bNoRange;    /* MW_NORANGE=1: no range reads (point reads instead) */ /* MW_CHECKCOMMIT=1: after each successful commit, check the committer's whole view */  /* MW_CHECKSNAP=1: full-table check at every snapshot (debug; changes conflicts) */   /* MW_TRACE=1: log every statement and commit to stderr */
#define TRACE(...) do{ if( bTrace ) fprintf(stderr, __VA_ARGS__); }while(0)

/* MW_LOG=1: print sqlite3_log() messages (upstream names the conflict page). */
static void logCb(void *pArg, int iErr, const char *zMsg){
  (void)pArg; fprintf(stderr, "LOG %d op=%ld: %s\n", iErr, iOp, zMsg);
}

static void die(const char *z){
  fprintf(stderr, "FAIL op=%ld: %s\n", iOp, z);
  exit(1);
}

/* The transaction's own view of key k. */
static const char *viewOf(Conn *c, int k){
  int i;
  for(i=c->nWr-1; i>=0; i--) if( c->aWr[i].k==k ) return c->aWr[i].v;
  return c->aSnap[k];
}

static void fullCheck(Conn *c, const char *zWhen);
static void observe(Conn *c, int lo, int hi);
static void snap(Conn *c){
  if( !c->hasSnap ){
    TRACE("op %ld conn %d SNAPSHOT\n", iOp, (int)(c-C));
    if( bCheckSnap ){
      memcpy(c->aSnap, aCommitted, sizeof(aCommitted));
      c->hasSnap = 1;
      fullCheck(c, "at snapshot");
      observe(c, 1, NKEYS);
      return;
    }
    memcpy(c->aSnap, aCommitted, sizeof(aCommitted));
    c->hasSnap = 1;
  }
}

static void observe(Conn *c, int lo, int hi){
  if( c->nObs>=MAXOBS ){ c->bObsOverflow = 1; return; }
  c->aObs[c->nObs].lo = lo; c->aObs[c->nObs].hi = hi; c->nObs++;
}

static void addWrite(Conn *c, int k, char *v){
  if( c->nWr>=MAXWR ) die("write set overflow (raise MAXWR)");
  c->aWr[c->nWr].k = k; c->aWr[c->nWr].v = v; c->nWr++;
}

static char *mkVal(Conn *c, int op){
  char *z = malloc(48);
  snprintf(z, 48, "c%d.t%d.o%d.%ld", (int)(c-C), c->iTxn, op, iOp);
  return z;
}

static int stepStmt(Conn *c, const char *zSql, int k, const char *zV){
  sqlite3_stmt *p;
  int rc;
  if( sqlite3_prepare_v2(c->db, zSql, -1, &p, 0) ) die(sqlite3_errmsg(c->db));
  sqlite3_bind_int(p, 1, k);
  if( zV ) sqlite3_bind_text(p, 2, zV, -1, SQLITE_STATIC);
  rc = sqlite3_step(p);
  sqlite3_finalize(p);
  return rc;
}

static void opPointRead(Conn *c){
  int k = 1 + rndn(NKEYS);
  sqlite3_stmt *p;
  const char *want, *got = 0;
  char buf[64];
  snap(c);
  sqlite3_prepare_v2(c->db, "SELECT v FROM t WHERE k=?", -1, &p, 0);
  sqlite3_bind_int(p, 1, k);
  if( sqlite3_step(p)==SQLITE_ROW ){
    snprintf(buf, sizeof(buf), "%s", (const char*)sqlite3_column_text(p, 0));
    got = buf;
  }
  sqlite3_finalize(p);
  want = viewOf(c, k);
  if( (want==0)!=(got==0) || (want && strcmp(want, got)) ){
    char e[200];
    snprintf(e, sizeof(e), "conn %d point read k=%d: want %s got %s",
             (int)(c-C), k, want?want:"(absent)", got?got:"(absent)");
    die(e);
  }
  observe(c, k, k);
}

static void opRangeRead(Conn *c){
  int lo = 1 + rndn(NKEYS), hi = lo + rndn(40), k;
  sqlite3_stmt *p;
  int want = lo;
  if( hi>NKEYS ) hi = NKEYS;
  snap(c);
  sqlite3_prepare_v2(c->db, "SELECT k, v FROM t WHERE k BETWEEN ? AND ? ORDER BY k",
                     -1, &p, 0);
  sqlite3_bind_int(p, 1, lo);
  sqlite3_bind_int(p, 2, hi);
  while( sqlite3_step(p)==SQLITE_ROW ){
    int gk = sqlite3_column_int(p, 0);
    const char *gv = (const char*)sqlite3_column_text(p, 1);
    for(k=want; k<gk; k++){
      if( viewOf(c, k) ){
        char e[160];
        snprintf(e, sizeof(e), "conn %d range [%d,%d]: key %d missing",
                 (int)(c-C), lo, hi, k);
        die(e);
      }
    }
    if( !viewOf(c, gk) || strcmp(viewOf(c, gk), gv) ){
      char e[200];
      snprintf(e, sizeof(e), "conn %d range [%d,%d]: key %d want %s got %s",
               (int)(c-C), lo, hi, gk, viewOf(c,gk)?viewOf(c,gk):"(absent)", gv);
      die(e);
    }
    want = gk+1;
  }
  sqlite3_finalize(p);
  for(k=want; k<=hi; k++){
    if( viewOf(c, k) ){
      char e[160];
      snprintf(e, sizeof(e), "conn %d range [%d,%d]: trailing key %d missing",
               (int)(c-C), lo, hi, k);
      die(e);
    }
  }
  TRACE("op %ld conn %d RANGE [%d,%d]\n", iOp, (int)(c-C), lo, hi);
  observe(c, lo, hi);
}

static void opWrite(Conn *c){
  int k = 1 + rndn(NKEYS), rc;
  int op = c->nWr;
  snap(c);
  observe(c, k, k);                 /* the statement finds / probes the row */
  if( viewOf(c, k)==0 ){
    char *v = mkVal(c, op);
    TRACE("op %ld conn %d INSERT %d=%s\n", iOp, (int)(c-C), k, v);
    rc = stepStmt(c, "INSERT INTO t(k,v) VALUES(?,?)", k, v);
    if( rc!=SQLITE_DONE ) die(sqlite3_errmsg(c->db));
    addWrite(c, k, v);
  }else if( rndn(4)==0 ){
    TRACE("op %ld conn %d DELETE %d\n", iOp, (int)(c-C), k);
    rc = stepStmt(c, "DELETE FROM t WHERE k=?", k, 0);
    if( rc!=SQLITE_DONE ) die(sqlite3_errmsg(c->db));
    addWrite(c, k, 0);
  }else{
    char *v = mkVal(c, op);
    TRACE("op %ld conn %d UPDATE %d=%s\n", iOp, (int)(c-C), k, v);
    rc = stepStmt(c, "UPDATE t SET v=?2 WHERE k=?1", k, v);
    if( rc!=SQLITE_DONE ) die(sqlite3_errmsg(c->db));
    addWrite(c, k, v);
  }
}

/* Did anything this transaction observed change since its snapshot? */
static int observedChanged(Conn *c){
  int i, k;
  if( c->bObsOverflow ) return 1;
  for(i=0; i<c->nObs; i++){
    for(k=c->aObs[i].lo; k<=c->aObs[i].hi; k++){
      if( c->aSnap[k]!=aCommitted[k] ) return 1;
    }
  }
  return 0;
}

static void endTxn(Conn *c){
  c->inTxn = 0; c->hasSnap = 0; c->nObs = 0; c->bObsOverflow = 0; c->nWr = 0;
}

static void opCommit(Conn *c){
  int rc, ext, changed, i;
  if( !c->hasSnap ){                 /* empty transaction */
    sqlite3_exec(c->db, "COMMIT", 0, 0, 0);
    endTxn(c);
    return;
  }
  /* A read-only transaction serialises at its SNAPSHOT, not at its commit:
  ** everything it read was checked against that snapshot already, and it has
  ** nothing to validate (BEGIN CONCURRENT does not).  The commit-order rule
  ** below applies to transactions that wrote. */
  changed = c->nWr>0 ? observedChanged(c) : 0;
  rc = sqlite3_exec(c->db, "COMMIT", 0, 0, 0);
  TRACE("op %ld conn %d COMMIT txn %d -> rc=%d (model changed=%d)\n", iOp, (int)(c-C), c->iTxn, rc, changed);
  ext = sqlite3_extended_errcode(c->db);
  if( rc==SQLITE_OK ){
    if( changed ){
      char e[200];
      int j, kk;
      for(j=0; j<c->nObs; j++) for(kk=c->aObs[j].lo; kk<=c->aObs[j].hi; kk++){
        if( c->aSnap[kk]!=aCommitted[kk] ){
          fprintf(stderr, "  observed [%d,%d]: key %d snap=%s committed=%s\n",
                  c->aObs[j].lo, c->aObs[j].hi, kk,
                  c->aSnap[kk]?c->aSnap[kk]:"(absent)",
                  aCommitted[kk]?aCommitted[kk]:"(absent)");
        }
      }
      fprintf(stderr, "  txn: %d observations, %d writes:", c->nObs, c->nWr);
      for(j=0; j<c->nWr; j++) fprintf(stderr, " %d=%s", c->aWr[j].k, c->aWr[j].v?c->aWr[j].v:"DEL");
      fprintf(stderr, "\n");
      snprintf(e, sizeof(e), "conn %d txn %d COMMITTED although a value it read "
               "was changed by a transaction that committed after its snapshot "
               "(not serialisable: lost update / write skew)", (int)(c-C), c->iTxn);
      die(e);
    }
    for(i=0; i<c->nWr; i++) aCommitted[c->aWr[i].k] = c->aWr[i].v;
    nCommit++;
    if( bCheckCommit ){
      /* A plain (not CONCURRENT) read transaction: observes nothing that any
      ** concurrent validation sees. */
      endTxn(c);
      sqlite3_exec(c->db, "BEGIN", 0, 0, 0);
      memcpy(c->aSnap, aCommitted, sizeof(aCommitted));
      c->hasSnap = 1;
      fullCheck(c, "right after its own commit");
      sqlite3_exec(c->db, "COMMIT", 0, 0, 0);
    }
  }else if( (rc&0xff)==SQLITE_BUSY ){
    if( ext!=SQLITE_BUSY_SNAPSHOT && rc!=SQLITE_BUSY ){
      die(sqlite3_errmsg(c->db));
    }
    if( changed ) nTrueConflict++; else nFalseConflict++;
    if( sqlite3_exec(c->db, "ROLLBACK", 0, 0, 0) ) die(sqlite3_errmsg(c->db));
  }else{
    die(sqlite3_errmsg(c->db));
  }
  endTxn(c);
}

static void fullCheck(Conn *c, const char *zWhen){
  sqlite3_stmt *p;
  int k, want = 1;
  sqlite3_prepare_v2(c->db, "SELECT k, v FROM t ORDER BY k", -1, &p, 0);
  while( sqlite3_step(p)==SQLITE_ROW ){
    int gk = sqlite3_column_int(p, 0);
    const char *gv = (const char*)sqlite3_column_text(p, 1);
    for(k=want; k<gk; k++) if( viewOf(c,k) ){
      char e[200]; snprintf(e, sizeof(e), "conn %d %s: key %d missing (want %s)", (int)(c-C), zWhen, k, viewOf(c,k)); die(e);
    }
    if( !viewOf(c,gk) || strcmp(viewOf(c,gk), gv) ){
      char e[200]; snprintf(e, sizeof(e), "conn %d %s: key %d want %s got %s", (int)(c-C), zWhen, gk,
               viewOf(c,gk)?viewOf(c,gk):"(absent)", gv); die(e);
    }
    want = gk+1;
  }
  sqlite3_finalize(p);
  for(k=want; k<=NKEYS; k++) if( viewOf(c,k) ){
    char e[160]; snprintf(e, sizeof(e), "conn %d %s: trailing key %d missing", (int)(c-C), zWhen, k); die(e);
  }
}

static void finalCheck(Conn *c){
  sqlite3_stmt *p;
  int k, want = 1;
  sqlite3_prepare_v2(c->db, "SELECT k, v FROM t ORDER BY k", -1, &p, 0);
  while( sqlite3_step(p)==SQLITE_ROW ){
    int gk = sqlite3_column_int(p, 0);
    const char *gv = (const char*)sqlite3_column_text(p, 1);
    for(k=want; k<gk; k++) if( aCommitted[k] ) die("final scan: committed key missing");
    if( !aCommitted[gk] || strcmp(aCommitted[gk], gv) ){
      char e[160];
      snprintf(e, sizeof(e), "final scan conn %d: key %d want %s got %s", (int)(c-C),
               gk, aCommitted[gk]?aCommitted[gk]:"(absent)", gv);
      die(e);
    }
    want = gk+1;
  }
  sqlite3_finalize(p);
  for(k=want; k<=NKEYS; k++) if( aCommitted[k] ) die("final scan: trailing key missing");
  sqlite3_prepare_v2(c->db, "PRAGMA integrity_check", -1, &p, 0);
  if( sqlite3_step(p)!=SQLITE_ROW || strcmp((const char*)sqlite3_column_text(p,0), "ok") ){
    die("integrity_check");
  }
  sqlite3_finalize(p);
}

int main(int argc, char **argv){
  long nOps;
  int i, k;
  const char *zPragmas;
  if( argc<5 ){
    fprintf(stderr, "usage: %s DB SEED NOPS NCONN [PRAGMAS]\n", argv[0]);
    return 2;
  }
  zDbPath = argv[1];
  rngState = strtoull(argv[2], 0, 10)*0x9E3779B97F4A7C15ull + 1;
  nOps = atol(argv[3]);
  nConn = atoi(argv[4]);
  if( nConn<2 || nConn>MAXCONN ) nConn = 3;
  zPragmas = argc>5 ? argv[5] : "";
  bTrace = getenv("MW_TRACE")!=0;
  bCheckSnap = getenv("MW_CHECKSNAP")!=0;
  bCheckCommit = getenv("MW_CHECKCOMMIT")!=0;
  bNoRange = getenv("MW_NORANGE")!=0;
  if( getenv("MW_LOG") ) sqlite3_config(SQLITE_CONFIG_LOG, logCb, (void*)0);
  { char b[600];
    snprintf(b, sizeof(b), "%s", zDbPath); remove(b);
    snprintf(b, sizeof(b), "%s-wal", zDbPath); remove(b);
    snprintf(b, sizeof(b), "%s-shm", zDbPath); remove(b); }

  for(i=0; i<nConn; i++){
    if( sqlite3_open_v2(zDbPath, &C[i].db,
          SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_PRIVATECACHE, 0) ){
      die("open");
    }
    sqlite3_exec(C[i].db, "PRAGMA journal_mode=wal", 0, 0, 0);
    if( zPragmas[0] && sqlite3_exec(C[i].db, zPragmas, 0, 0, 0) ){
      die(sqlite3_errmsg(C[i].db));
    }
    if( i==0 ){
      sqlite3_exec(C[0].db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT)", 0, 0, 0);
      sqlite3_exec(C[0].db, "BEGIN", 0, 0, 0);
      for(k=1; k<=NKEYS; k+=2){
        char *v = malloc(16);
        snprintf(v, 16, "init%d", k);
        if( stepStmt(&C[0], "INSERT INTO t(k,v) VALUES(?,?)", k, v)!=SQLITE_DONE ){
          die(sqlite3_errmsg(C[0].db));
        }
        aCommitted[k] = v;
      }
      sqlite3_exec(C[0].db, "COMMIT", 0, 0, 0);
    }
  }

  for(iOp=0; iOp<nOps; iOp++){
    Conn *c = &C[rndn(nConn)];
    if( !c->inTxn ){
      if( sqlite3_exec(c->db, "BEGIN CONCURRENT", 0, 0, 0) ) die(sqlite3_errmsg(c->db));
      TRACE("op %ld conn %d BEGIN txn %d\n", iOp, (int)(c-C), c->iTxn+1);
      c->inTxn = 1; c->iTxn++; nTxn++;
      c->nLeft = 1 + rndn(6);
      continue;
    }
    if( c->nLeft--<=0 ){ opCommit(c); continue; }
    switch( rndn(10) ){
      case 0: case 1: case 2: opPointRead(c); break;
      case 3: if( bNoRange ) opPointRead(c); else opRangeRead(c); break;
      default: opWrite(c); break;
    }
  }
  for(i=0; i<nConn; i++) if( C[i].inTxn ) opCommit(&C[i]);
  for(i=0; i<nConn; i++) finalCheck(&C[i]);
  printf("OK %ld ops, %d conns: %ld txns, %ld commits, %ld true conflicts, "
         "%ld FALSE conflicts (%.1f%% of refusals)\n", nOps, nConn, nTxn, nCommit,
         nTrueConflict, nFalseConflict,
         (nTrueConflict+nFalseConflict) ?
           100.0*nFalseConflict/(nTrueConflict+nFalseConflict) : 0.0);
  { long long nVal = 0, nRow = 0, nOps = 0;
    for(i=0; i<nConn; i++){
      sqlite3_stmt *p;
      if( sqlite3_prepare_v2(C[i].db, "PRAGMA bf_cache_stats", -1, &p, 0) ) break;
      while( sqlite3_step(p)==SQLITE_ROW ){
        const char *zN = (const char*)sqlite3_column_text(p, 0);
        if( !strcmp(zN, "conc_validated") ) nVal += sqlite3_column_int64(p, 1);
        if( !strcmp(zN, "conc_conflicts") ) nRow += sqlite3_column_int64(p, 1);
        if( !strcmp(zN, "conc_ops_tested") ) nOps += sqlite3_column_int64(p, 1);
      }
      sqlite3_finalize(p);
    }
    if( nVal ){
      printf("   BF: %lld commits reached row-level validation, %lld refused there "
             "(%lld refused earlier, by the page check); %lld ops tested\n",
             nVal, nRow, (nTrueConflict+nFalseConflict)-nRow, nOps);
    }
  }
  for(i=0; i<nConn; i++) sqlite3_close(C[i].db);
  return 0;
}
