/*
** Power-loss oracle driver (D2, 2026-09-24).
**
** A SIGKILL cannot lose a write that reached the kernel, so it cannot test
** PRAGMA synchronous at all.  This driver runs a SQL script through a shim VFS
** that remembers, per file, what every write since that file's last xSync
** overwrote.  At a chosen event it "cuts the power": the unsynced writes are
** reverted -- all of them, a random subset, or torn at 512-byte sector
** granularity -- and the process _exit()s without closing anything.  The
** database is then left exactly as a power loss at that instant could leave it,
** under the usual model: synced data survives, unsynced data may or may not,
** in any combination.
**
** Input (stdin): one SQL statement per line, or ".print TEXT".
** Output (stdout, flushed per line):
**   TEXT [D]     for every .print line, printed after everything before it ran;
**                " D" is appended when no tracked file has an unsynced write at
**                that moment, i.e. everything acknowledged so far is durable;
**   SYNC <n>     after every completed xSync on the WAL or database file.
**   ERR <msg>    for a statement that failed.
** The oracle takes "acknowledged" from the TEXT markers and "durable" from the
** D flag.  (Counting SYNC lines instead is wrong: SQLite syncs the WAL HEADER
** before a first commit's frames are written, which covers nothing.)
**
** Environment:
**   PL_CUT_AT=K    cut the power at the K-th counted event (0 = never)
**   PL_MODE=all|subset|torn     what the cut does to unsynced writes (default all)
**   PL_SEED=S      randomness for subset/torn
**   PL_COUNT=1     at exit, print "EVENTS <n>" (dry run to size PL_CUT_AT)
**   PL_PSOW=0      report no device capabilities (sector-padded commits)
** Events are xWrite, xSync and xTruncate calls on the main database and WAL.
**
** Build (from bench/powerloss, against a BF amalgamation):
**   cc -O2 -DSQLITE_BF_INSERT_BUFFERING -DSQLITE_THREADSAFE=0 -I<dir with sqlite3.h>
**      -o plvfs plvfs.c <dir>/sqlite3.c -lm
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct UndoRec UndoRec;
struct UndoRec {
  sqlite3_int64 iOff;       /* where the write landed */
  int n;                    /* bytes written */
  int nOld;                 /* bytes of old content that existed (<= n) */
  unsigned char *aOld;      /* old content of [iOff, iOff+nOld) */
  sqlite3_int64 szBefore;   /* file size before this write/truncate */
  int isTrunc;              /* a truncate: aOld holds the cut tail */
  UndoRec *pNext;           /* older record */
};

typedef struct PlFile PlFile;
struct PlFile {
  sqlite3_file base;        /* must be first */
  sqlite3_file *pReal;      /* the real file, allocated right after us */
  int tracked;              /* main db or WAL: writes are undoable */
  int isWal;
  UndoRec *pUndo;           /* newest first */
  PlFile *pNextOpen;
};

static sqlite3_vfs *gReal;
static sqlite3_vfs gVfs;
static PlFile *gOpen;
static long gEvents, gCutAt;
static int gMode;           /* 0 all, 1 subset, 2 torn */
static unsigned int gRand;
static int gCount;

static unsigned int plRand(void){
  gRand = gRand*1103515245u + 12345u;
  return (gRand>>16) & 0x7fff;
}

static void plFreeUndo(PlFile *p){
  UndoRec *r = p->pUndo;
  while( r ){ UndoRec *n = r->pNext; free(r->aOld); free(r); r = n; }
  p->pUndo = 0;
}

/* Revert unsynced writes, newest first, then die without cleanup. */
static void plCut(void){
  PlFile *p;
  for(p=gOpen; p; p=p->pNextOpen){
    UndoRec *r;
    if( !p->tracked ) continue;
    for(r=p->pUndo; r; r=r->pNext){
      if( r->isTrunc ){
        /* The truncate is lost: put the tail back. */
        if( r->nOld>0 ){
          p->pReal->pMethods->xWrite(p->pReal, r->aOld, r->nOld, r->iOff);
        }
        continue;
      }
      if( gMode==1 && (plRand()&1) ) continue;          /* this one survived */
      if( gMode==2 ){
        /* Torn: sectors from the start survive, the rest revert. */
        int nSec = (r->n+511)/512, keep = (int)(plRand() % (unsigned)(nSec+1));
        int from = keep*512;
        if( from < r->nOld ){
          p->pReal->pMethods->xWrite(p->pReal, r->aOld+from, r->nOld-from,
                                     r->iOff+from);
        }
        if( from < r->n && r->iOff + r->n > r->szBefore ){
          sqlite3_int64 cut = r->iOff+from > r->szBefore ? r->iOff+from : r->szBefore;
          p->pReal->pMethods->xTruncate(p->pReal, cut);
        }
        continue;
      }
      if( r->nOld>0 ){
        p->pReal->pMethods->xWrite(p->pReal, r->aOld, r->nOld, r->iOff);
      }
      if( r->iOff + r->n > r->szBefore ){
        p->pReal->pMethods->xTruncate(p->pReal, r->szBefore);
      }
    }
  }
  printf("CUT %ld\n", gEvents);
  fflush(stdout);
  _exit(0);
}

static void plEvent(void){
  gEvents++;
  if( gCutAt>0 && gEvents>=gCutAt ) plCut();
}

static int plClose(sqlite3_file *f){
  PlFile *p = (PlFile*)f, **pp;
  int rc = p->pReal->pMethods->xClose(p->pReal);
  plFreeUndo(p);
  for(pp=&gOpen; *pp; pp=&(*pp)->pNextOpen){
    if( *pp==p ){ *pp = p->pNextOpen; break; }
  }
  return rc;
}
static int plRead(sqlite3_file *f, void *z, int n, sqlite3_int64 o){
  PlFile *p = (PlFile*)f;
  return p->pReal->pMethods->xRead(p->pReal, z, n, o);
}
static int plWrite(sqlite3_file *f, const void *z, int n, sqlite3_int64 o){
  PlFile *p = (PlFile*)f;
  if( p->tracked ){
    sqlite3_int64 sz = 0;
    UndoRec *r = (UndoRec*)calloc(1, sizeof(*r));
    if( !r ) return SQLITE_NOMEM;
    p->pReal->pMethods->xFileSize(p->pReal, &sz);
    r->iOff = o; r->n = n; r->szBefore = sz;
    if( o < sz ){
      r->nOld = (int)((o+n <= sz) ? n : sz-o);
      r->aOld = (unsigned char*)malloc(r->nOld);
      if( !r->aOld ){ free(r); return SQLITE_NOMEM; }
      if( p->pReal->pMethods->xRead(p->pReal, r->aOld, r->nOld, o)!=SQLITE_OK ){
        memset(r->aOld, 0, r->nOld);
      }
    }
    r->pNext = p->pUndo; p->pUndo = r;
    plEvent();   /* the cut may land BEFORE this write happens */
  }
  return p->pReal->pMethods->xWrite(p->pReal, z, n, o);
}
static int plTruncate(sqlite3_file *f, sqlite3_int64 size){
  PlFile *p = (PlFile*)f;
  if( p->tracked ){
    sqlite3_int64 sz = 0;
    p->pReal->pMethods->xFileSize(p->pReal, &sz);
    if( size < sz ){
      UndoRec *r = (UndoRec*)calloc(1, sizeof(*r));
      if( !r ) return SQLITE_NOMEM;
      r->isTrunc = 1; r->iOff = size; r->szBefore = sz;
      r->nOld = (int)(sz-size);
      r->aOld = (unsigned char*)malloc(r->nOld);
      if( !r->aOld ){ free(r); return SQLITE_NOMEM; }
      p->pReal->pMethods->xRead(p->pReal, r->aOld, r->nOld, size);
      r->pNext = p->pUndo; p->pUndo = r;
    }
    plEvent();
  }
  return p->pReal->pMethods->xTruncate(p->pReal, size);
}
static int plSync(sqlite3_file *f, int flags){
  PlFile *p = (PlFile*)f;
  int rc;
  if( p->tracked ) plEvent();      /* a cut here: this sync never happened */
  rc = p->pReal->pMethods->xSync(p->pReal, flags);
  if( rc==SQLITE_OK && p->tracked ){
    plFreeUndo(p);                 /* everything written so far is durable */
    printf("SYNC %s %ld\n", p->isWal ? "wal" : "db", gEvents);
    fflush(stdout);
  }
  return rc;
}
static int plFileSize(sqlite3_file *f, sqlite3_int64 *s){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xFileSize(p->pReal, s);
}
static int plLock(sqlite3_file *f, int l){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xLock(p->pReal, l);
}
static int plUnlock(sqlite3_file *f, int l){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xUnlock(p->pReal, l);
}
static int plCheck(sqlite3_file *f, int *r){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xCheckReservedLock(p->pReal, r);
}
static int plFcntl(sqlite3_file *f, int op, void *a){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xFileControl(p->pReal, op, a);
}
static int plSector(sqlite3_file *f){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xSectorSize(p->pReal);
}
static int gPsow = 1;
static int plDevChar(sqlite3_file *f){
  /* Default: pass the real characteristics through, so the run takes the path
  ** production takes (unix reports POWERSAFE_OVERWRITE, which is what lets a
  ** record-only commit ride its last record frame).  PL_PSOW=0 claims nothing,
  ** forcing SQLite's most conservative path: commits padded to a sector. */
  PlFile *p = (PlFile*)f;
  return gPsow ? p->pReal->pMethods->xDeviceCharacteristics(p->pReal) : 0;
}
static int plShmMap(sqlite3_file *f, int i, int sz, int w, void volatile **pp){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xShmMap(p->pReal, i, sz, w, pp);
}
static int plShmLock(sqlite3_file *f, int o, int n, int fl){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xShmLock(p->pReal, o, n, fl);
}
static void plShmBarrier(sqlite3_file *f){
  PlFile *p = (PlFile*)f; p->pReal->pMethods->xShmBarrier(p->pReal);
}
static int plShmUnmap(sqlite3_file *f, int d){
  PlFile *p = (PlFile*)f; return p->pReal->pMethods->xShmUnmap(p->pReal, d);
}

static const sqlite3_io_methods gMethods = {
  2, plClose, plRead, plWrite, plTruncate, plSync, plFileSize, plLock, plUnlock,
  plCheck, plFcntl, plSector, plDevChar, plShmMap, plShmLock, plShmBarrier,
  plShmUnmap, 0, 0
};

static int plOpen(sqlite3_vfs *v, const char *zName, sqlite3_file *f, int flags,
                  int *pOut){
  PlFile *p = (PlFile*)f;
  int rc;
  (void)v;
  memset(p, 0, sizeof(*p));
  p->pReal = (sqlite3_file*)&p[1];
  rc = gReal->xOpen(gReal, zName, p->pReal, flags, pOut);
  if( rc!=SQLITE_OK ){ p->base.pMethods = 0; return rc; }
  p->tracked = (flags & (SQLITE_OPEN_MAIN_DB|SQLITE_OPEN_WAL))!=0;
  p->isWal = (flags & SQLITE_OPEN_WAL)!=0;
  p->base.pMethods = &gMethods;
  p->pNextOpen = gOpen; gOpen = p;
  return SQLITE_OK;
}
static int plDelete(sqlite3_vfs *v, const char *z, int s){
  (void)v; return gReal->xDelete(gReal, z, s);
}
static int plAccess(sqlite3_vfs *v, const char *z, int fl, int *r){
  (void)v; return gReal->xAccess(gReal, z, fl, r);
}
static int plFull(sqlite3_vfs *v, const char *z, int n, char *o){
  (void)v; return gReal->xFullPathname(gReal, z, n, o);
}
static void *plDlOpen(sqlite3_vfs *v, const char *z){ (void)v; return gReal->xDlOpen(gReal, z); }
static void plDlError(sqlite3_vfs *v, int n, char *z){ (void)v; gReal->xDlError(gReal, n, z); }
static void (*plDlSym(sqlite3_vfs *v, void *h, const char *z))(void){
  (void)v; return gReal->xDlSym(gReal, h, z);
}
static void plDlClose(sqlite3_vfs *v, void *h){ (void)v; gReal->xDlClose(gReal, h); }
static int plRandomness(sqlite3_vfs *v, int n, char *z){ (void)v; return gReal->xRandomness(gReal, n, z); }
static int plSleep(sqlite3_vfs *v, int u){ (void)v; return gReal->xSleep(gReal, u); }
static int plTime(sqlite3_vfs *v, double *t){ (void)v; return gReal->xCurrentTime(gReal, t); }
static int plLastErr(sqlite3_vfs *v, int n, char *z){ (void)v; return gReal->xGetLastError(gReal, n, z); }
static int plTime64(sqlite3_vfs *v, sqlite3_int64 *t){ (void)v; return gReal->xCurrentTimeInt64(gReal, t); }

int main(int argc, char **argv){
  sqlite3 *db = 0;
  char *zLine = 0;
  size_t nCap = 0;
  const char *e;
  if( argc<2 ){ fprintf(stderr, "usage: plvfs DB < script\n"); return 2; }
  gCutAt = (e=getenv("PL_CUT_AT")) ? atol(e) : 0;
  gRand  = (e=getenv("PL_SEED")) ? (unsigned)atol(e) : 1u;
  gCount = (e=getenv("PL_COUNT")) ? atoi(e) : 0;
  gPsow  = (e=getenv("PL_PSOW")) ? atoi(e) : 1;
  e = getenv("PL_MODE");
  gMode = (e && !strcmp(e,"subset")) ? 1 : (e && !strcmp(e,"torn")) ? 2 : 0;

  gReal = sqlite3_vfs_find(0);
  gVfs = *gReal;
  gVfs.zName = "plvfs";
  gVfs.szOsFile = (int)sizeof(PlFile) + gReal->szOsFile;
  gVfs.xOpen = plOpen; gVfs.xDelete = plDelete; gVfs.xAccess = plAccess;
  gVfs.xFullPathname = plFull; gVfs.xDlOpen = plDlOpen; gVfs.xDlError = plDlError;
  gVfs.xDlSym = plDlSym; gVfs.xDlClose = plDlClose; gVfs.xRandomness = plRandomness;
  gVfs.xSleep = plSleep; gVfs.xCurrentTime = plTime; gVfs.xGetLastError = plLastErr;
  gVfs.xCurrentTimeInt64 = plTime64;
  gVfs.xSetSystemCall = 0; gVfs.xGetSystemCall = 0; gVfs.xNextSystemCall = 0;
  sqlite3_vfs_register(&gVfs, 1);

  if( sqlite3_open(argv[1], &db)!=SQLITE_OK ){
    printf("ERR open %s\n", sqlite3_errmsg(db)); return 1;
  }
  while( getline(&zLine, &nCap, stdin) > 0 ){
    size_t n = strlen(zLine);
    while( n>0 && (zLine[n-1]=='\n' || zLine[n-1]=='\r') ) zLine[--n] = 0;
    if( n==0 ) continue;
    if( !strncmp(zLine, ".print ", 7) ){
      PlFile *p;
      int dirty = 0;
      for(p=gOpen; p; p=p->pNextOpen) if( p->tracked && p->pUndo ) dirty = 1;
      printf("%s%s\n", zLine+7, dirty ? "" : " D"); fflush(stdout); continue;
    }
    {
      char *zErr = 0;
      if( sqlite3_exec(db, zLine, 0, 0, &zErr)!=SQLITE_OK ){
        printf("ERR %s\n", zErr ? zErr : "?"); fflush(stdout);
        sqlite3_free(zErr);
      }
    }
  }
  if( gCount ){ printf("EVENTS %ld\n", gEvents); fflush(stdout); }
  _exit(0);   /* no close: the close-time checkpoint is not part of the run */
}
