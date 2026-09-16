/*
** 2024 January 01
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** BF-Tree btree integration hooks.
**
** Phase 1 keying: BF read-cache records are keyed by the LEAF page the cursor
** descends to (bfCursorLeafPgno), giving one mini-page per hot leaf — buffer
** capacity proportional to the working set and range locality.  SQLite's own
** descent through the inner pages is the routing structure; the existing
** pgno-keyed mapping table works unchanged.  The per-leaf key is read by the
** moveto BF hook, payload fetch, promotion, phantom and write-through cache,
** all AFTER the descent has reached a leaf (a leaf may split — Stage 1.4 will
** re-route records; stale wrong-leaf entries are merely unreachable, never
** wrong, because reads only ever consult the leaf the key descends to).
**
** Cache model (Phase 0/1 — write-through, no durable buffering):
**   - INSERT/UPDATE → base-page write, then the record is mirrored into the
**     leaf's mini-page as BFOP_CACHE (clean).
**   - DELETE → base-page delete, then a clean BFOP_PHANTOM on the leaf
**     suppresses any stale cached row for that key.
**   - SELECT point lookup → leaf mini-page check serves CACHE / reports
**     PHANTOM-as-absent; a miss reads the base page and may promote the row.
**   - No BFOP_INSERT/BFOP_DELETE dirty records exist in this phase, so flush,
**     scan-prep and eviction never have buffered writes to apply.  Durable
**     write-buffering (per-leaf) returns in later stages.
*/
#include "sqliteInt.h"
#include "btreeInt.h"
#include "bf_cache.h"
#if defined(SQLITE_BF_INSERT_BUFFERING) && !defined(SQLITE_OMIT_BF_CACHE)
#include "bf_wal.h"   /* record-batch codec for commit-time WAL logging */
#endif

#ifdef SQLITE_BF_DEBUG
#include <fcntl.h>
#include <unistd.h>
static void bfBtreeTrace(const char *zTag, const void *pPtr, int n){
  char zBuf[256];
  int nOut = snprintf(zBuf, sizeof(zBuf), "bfbtree: %s ptr=%p size=%d\n", zTag, pPtr, n);
  int fd = open("/home/yawd/Projects/tfg-stuff/sqlite/build/bf-allocs.log",
                O_WRONLY|O_CREAT|O_APPEND, 0644);
  if( fd>=0 ){ write(fd, zBuf, nOut); close(fd); }
}
#define BF_BT_TRACE(t,p,n)  bfBtreeTrace(t,p,n)
#else
#define BF_BT_TRACE(t,p,n)  ((void)0)
#endif

#ifndef SQLITE_OMIT_BF_CACHE

/* -------------------------------------------------------------------------
** Internal helpers
** ----------------------------------------------------------------------- */

static int btreeUsesBfCache(BtShared *pBt){
  if( !pBt || !sqlite3BfCacheEnabled() ) return 0;
  /* BF is unsupported on auto-vacuum databases (Stage 1.5 page-lifecycle).
  ** Auto/incremental vacuum moves pages to new page numbers via relocatePage,
  ** which would silently invalidate (or, for a dirty mini-page, corrupt) the
  ** per-leaf cache keyed by the old pgno.  Rather than hook relocatePage to
  ** re-key every entry, we declare the combination unsupported and disable BF
  ** here: no mini-page is ever created for an auto-vacuum DB, so relocatePage
  ** never observes one.  Every relocatePage call site is already gated on
  ** pBt->autoVacuum, so this single check fully closes the hole.  The base
  ** B-tree still works normally — the database just runs without BF caching.
  ** (See BF_TREE_DESIGN.md.) */
  if( pBt->autoVacuum ) return 0;
  return 1;
}

/*
** Get the BfCache for a cursor's B-tree.  Returns NULL when:
**   - BF is globally disabled
**   - No per-pager cache has been created yet
**   - bBypassActive is set (prevents re-entrant BF writes during flush)
**
** As a side-effect, lazily records the BtShared back-pointer in the cache.
*/
static BfCache *btreeGetBfCache(BtShared *pBt){
  BfCache *pBf;
  if( !pBt || !btreeUsesBfCache(pBt) ) return 0;
  pBf = sqlite3PagerGetBfCache(pBt->pPager);
  if( !pBf ) return 0;
  if( pBf->bBypassActive ) return 0;
  if( !pBf->pBtShared ) pBf->pBtShared = pBt;
  return pBf;
}

/*
** Encode a rowid as an 8-byte big-endian key (same encoding used across
** all BF record operations so lookups and writes are consistent).
*/
static void bfEncodeRowid(i64 rowid, u8 *pBuf){
  int i;
  for(i=7; i>=0; i--){ pBuf[i]=(u8)(rowid&0xff); rowid>>=8; }
}

/*
** Page number the BF per-leaf cache keys by (Phase 1 keying): the leaf the
** cursor is currently positioned on.  A record lives in the mini-page of the
** leaf that holds — or, for a phantom, would hold — the row, so the read
** check, payload fetch, promotion, phantom and write-through caches all agree
** on one key per row.  Returns 0 when the cursor is not validly positioned on
** a non-schema leaf; the caller must then refuse the BF op and use the base
** page (a cache miss is always safe).
*/
static u32 bfCursorLeafPgno(BtCursor *pCur){
  MemPage *pPage;
  if( !pCur || pCur->eState!=CURSOR_VALID ) return 0;
  /* Descent shortcut (Stage 1.6): the row is being served straight from the
  ** BF mini-page and the leaf was never read, so pCur->pPage is the PARENT
  ** interior page.  The leaf the record belongs to is recorded in bfLeaf. */
  if( (pCur->curFlags & BTCF_BfLeaf)!=0 ){
    return pCur->bfLeaf>1 ? pCur->bfLeaf : 0;
  }
  pPage = pCur->pPage;
  if( pPage==0 || !pPage->leaf || pPage->pgno<=1 ) return 0;
  return pPage->pgno;
}

/*
** Record the owning table's root page on the mini-page mapped under leaf
** pgno `leaf`.  The cache layer (sqlite3BfRecordWrite) creates new mini-pages
** with rootPgno defaulted to the map key (the leaf), because it cannot know
** the table root; the btree layer does.  Call this right after every
** leaf-keyed write so the flush machinery can open its replay cursor on the
** real root (bfFlushOneMiniPage) and group "all dirty leaves of table X".
** No-op if the entry is absent (write was refused) or root is invalid. */
static void bfTagLeafRoot(BfCache *pBf, u32 leaf, u32 root){
  BfMapEntry *pEntry;
  if( !pBf || leaf==0 || root<=1 ) return;
  pEntry = sqlite3BfMapLookup(pBf, leaf);
  if( pEntry && pEntry->locType==BF_LOC_MINI && pEntry->pPage ){
    ((BfMiniPage*)pEntry->pPage)->rootPgno = root;
  }
}

/* -------------------------------------------------------------------------
** Flush callbacks — apply BFOP_INSERT / BFOP_DELETE to base B-tree pages
** ----------------------------------------------------------------------- */

/*
** Context passed to the per-record apply callback.
*/
typedef struct BfApplyRecCtx BfApplyRecCtx;
struct BfApplyRecCtx {
  BtCursor *pCur;    /* write cursor positioned on the target table */
  int nApplied;      /* records successfully written */
  int nErrors;       /* records that failed (non-fatal) */
};

/*
** Apply one BFOP_INSERT or BFOP_DELETE record to the base B-tree.
** Called by sqlite3BfMiniPageIterate during a table flush.
*/
static int bfApplyOneRecord(void *pCtx, const u8 *pKey, int nKey,
                            const u8 *pVal, int nVal, u8 opType){
  BfApplyRecCtx *ctx = (BfApplyRecCtx*)pCtx;
  BtCursor *pCur = ctx->pCur;
  int rc;

  /* Only dirty records need to reach the base page. */
  if( opType==BFOP_CACHE || opType==BFOP_PHANTOM ) return 0;

  if( nKey!=8 ) return 0;  /* not a rowid key — skip */

  if( opType==BFOP_INSERT ){
    BtreePayload payload;
    i64 rowid = 0;
    int ki;
    for(ki=0; ki<8; ki++) rowid = (rowid<<8) | pKey[ki];

    memset(&payload, 0, sizeof(payload));
    payload.nKey  = rowid;
    payload.pData = (void*)pVal;
    payload.nData = nVal;
    payload.nZero = 0;

    rc = sqlite3BtreeInsert(pCur, &payload, 0, 0);
    if( rc==SQLITE_OK ){
      ctx->nApplied++;
    }else{
      ctx->nErrors++;
    }
  }else if( opType==BFOP_DELETE ){
    i64 rowid = 0;
    int ki, loc = 0;
    for(ki=0; ki<8; ki++) rowid = (rowid<<8) | pKey[ki];

    rc = sqlite3BtreeTableMoveto(pCur, rowid, 0, &loc);
    if( rc==SQLITE_OK && loc==0 ){
      rc = sqlite3BtreeDelete(pCur, 0);
    }
    if( rc==SQLITE_OK ){
      ctx->nApplied++;
    }else{
      ctx->nErrors++;
    }
  }
  return 0;  /* always continue iteration */
}

/*
** Flush all dirty records in a mini-page (keyed by pgnoRoot) to the
** base B-tree using a temporary write cursor.  Called by both the
** per-table pre-scan flush and the commit-time full flush.
**
** Returns SQLITE_OK on success (even if some records couldn't be applied —
** those remain dirty and will be retried on the next flush).
*/
static int bfFlushOneMiniPage(BfCache *pBf, Btree *pBtree, Pgno pgnoRoot,
                               BfMiniPage *pMini){
  BtCursor tmpCur;
  BfApplyRecCtx ctx;
  int rc;

  if( !pBf || !pBtree || pgnoRoot==0 || !pMini ) return SQLITE_OK;
  if( !sqlite3BfMiniPageIsDirty(pMini) ) return SQLITE_OK;
  /* A flush WRITES base pages, so it needs a write transaction.  Under a read
  ** transaction the bypass cursor's inserts all fail, yet the records would be
  ** marked clean below — with Phase-2 record logging (records live in the cache
  ** + their WAL record frames until a checkpoint materialises them) that would
  ** silently destroy committed rows: the next checkpoint sees nothing dirty,
  ** materialises nothing, and resets the WAL.  Read paths must merge instead of
  ** flushing (sqlite3BtreeFirst / bfMergeSeek); refuse here so a path that has
  ** no merge support degrades to "does not see buffered rows" rather than
  ** "loses them". */
  if( pBtree->inTrans!=TRANS_WRITE ) return SQLITE_OK;

#if defined(SQLITE_BF_INSERT_BUFFERING)
  /* Group commit ordering invariant: get any pending record batch into THIS
  ** transaction's frame stream before writing base pages, so replay sees the
  ** older record frame first and the newer page image last. */
  sqlite3BfBtreeGroupStagePending(pBtree);
#endif

  memset(&ctx, 0, sizeof(ctx));
  memset(&tmpCur, 0, (size_t)sqlite3BtreeCursorSize());

  /* Take the write lock the VDBE would have taken with OP_TableLock before
  ** opening a cursor on this table.  BF opens this one itself -- outside any
  ** statement -- so nothing else does it, and btreeCursor() asserts the lock is
  ** held: every SQLITE_DEBUG build aborts on the first flush
  ** ("hasSharedCacheTableLock(...) failed").  That matters beyond tidiness,
  ** because a debug build is the fastest diagnostic this project has -- it is
  ** what found the wal-index corruption bug, firing an assert immediately where
  ** the release build only produced a confusing SQLITE_CORRUPT several
  ** statements later.  sqlite3BtreeLockTable() is a no-op on a non-shareable
  ** btree, so this costs nothing in the common build. */
  rc = sqlite3BtreeLockTable(pBtree, (int)pgnoRoot, 1);
  if( rc!=SQLITE_OK ) return rc;

  pBf->bBypassActive = 1;
  pBf->pgnoRootForFlush = pgnoRoot;

  rc = sqlite3BtreeCursor(pBtree, pgnoRoot, BTREE_WRCSR, NULL/*rowid table*/,
                          &tmpCur);
  if( rc==SQLITE_OK ){
    ctx.pCur = &tmpCur;
    sqlite3BfMiniPageIterate(pMini, bfApplyOneRecord, &ctx);
    sqlite3BtreeCloseCursor(&tmpCur);
  }

  pBf->bBypassActive = 0;
  pBf->pgnoRootForFlush = 0;

  /* Mark applied records clean; leave failed ones dirty for retry. */
  if( ctx.nErrors==0 ){
    sqlite3BfMiniPageMarkClean(pMini);
    pBf->nMergeToBase += (u64)ctx.nApplied;
    if( pMini->flags & BF_MINI_F_STALE ){
      /* A balance moved this leaf's key range while we were replaying it, so
      ** every record left here describes keys the leaf may no longer own.  They
      ** are all CLEAN now, and dropping them mid-iteration would have lost
      ** writes -- do it now that the iteration is over. */
      pMini->flags &= ~BF_MINI_F_STALE;
      sqlite3BfMiniPageDropClean(pMini);
    }
  }

  return SQLITE_OK;
}

/*
** Callback for sqlite3BfMapIterate used by sqlite3BfBtreeFlushAllDirty.
*/
typedef struct BfFlushAllCtx BfFlushAllCtx;
struct BfFlushAllCtx {
  BfCache *pBf;
  Btree   *pBtree;
};


static int bfFlushAllCallback(void *pCtx, u32 pgno, BfMapEntry *pEntry){
  BfFlushAllCtx *ctx = (BfFlushAllCtx*)pCtx;
  BfMiniPage *pMini;

  if( pgno<=1 ) return 0;  /* never touch sqlite_schema or special tables */
  if( pEntry->locType!=BF_LOC_MINI ) return 0;
  pMini = (BfMiniPage*)pEntry->pPage;
  if( !pMini || !sqlite3BfMiniPageIsDirty(pMini) ) return 0;

  /* Mini-pages are keyed by LEAF pgno (Phase 1); the replay cursor must be
  ** opened on the owning TABLE root, recorded in the slab by bfTagLeafRoot. */
  if( pMini->rootPgno<=1 ) return 0;
  bfFlushOneMiniPage(ctx->pBf, ctx->pBtree, (Pgno)pMini->rootPgno, pMini);
  return 0;
}

/*
** Flush ALL dirty mini-pages in pBf to their base B-tree tables.
** Called at commit time before sqlite3PagerCommitPhaseOne.
*/
int sqlite3BfBtreeFlushAllDirty(Btree *p){
  BfCache *pBf;
  BfFlushAllCtx ctx;

  if( !p || !p->pBt ) return SQLITE_OK;
  pBf = sqlite3PagerGetBfCache(p->pBt->pPager);
  if( !pBf ) return SQLITE_OK;

  ctx.pBf    = pBf;
  ctx.pBtree = p;

  /* Without a write transaction nothing can be materialised (see
  ** bfFlushOneMiniPage); leave the records dirty rather than declaring them
  ** flushed. */
  if( p->inTrans!=TRANS_WRITE ) return SQLITE_OK;

  if( sqlite3BfDirtyListIterate(pBf, bfFlushAllCallback, &ctx) ){
    sqlite3BfMapIterate(pBf, bfFlushAllCallback, &ctx);   /* list overflowed */
  }
  pBf->bDirtyInserts = 0;  /* all buffered inserts materialised */
  return SQLITE_OK;
}

#if defined(SQLITE_BF_INSERT_BUFFERING)
/* -------------------------------------------------------------------------
** Phase 2 (WAL): commit-time record LOGGING (as opposed to flushing to base).
**
** Instead of merging dirty mini-page records into their base B-tree pages at
** commit (sqlite3BfBtreeFlushAllDirty, which forces 4 KB page-image WAL frames),
** we serialise each dirty-but-unlogged record into a physiological record-batch
** payload and stage it for the pending WAL commit (sqlite3PagerBfStage).  The
** records stay authoritative in the mini-page cache (still DIRTY) and are
** materialised to base lazily at checkpoint/eviction — that is the write win:
** a commit persists small records, not whole pages.
**
** Each logged record is marked (BF_KV logged flag) so a subsequent commit in the
** same WAL generation does not re-log it.  Marking happens as the record is
** appended; if any later step fails, the whole commit fails and btree rollback
** calls sqlite3BfBtreeClearCache, discarding the marks — so a mark can never
** outlive an uncommitted transaction.
** ----------------------------------------------------------------------- */
/*
** Group commit (Phase 2).  The open record batch lives on the BfCache, not on
** the stack of one commit, so consecutive transactions can pack their records
** into the SAME record frame.  With PRAGMA bf_group_commit=N a commit appends
** its records and returns without writing anything (the pager's
** "no page dirtied" early-out then makes the commit a genuine zero-byte
** operation); the N-th transaction stages the batch and its commit writes one
** record frame plus the single page-1 commit frame for the whole group.
**
** Durability trade: COMMIT returns before the records reach the WAL, so a crash
** loses at most the open group.  That is the paper-faithful setting; N<=1 keeps
** strict per-commit record logging.  Nothing is ever LOST to a clean shutdown:
** the records stay DIRTY in the mini-page, so if the batch is never written the
** close/checkpoint flush still materialises them into base pages.
**
** Ordering invariant: the open batch MUST be staged before any base-page
** materialisation of the same records (bfFlushOneMiniPage), because staged
** record frames are emitted AHEAD of the page images in the same WAL commit —
** replay then applies the older record first and the newer page image last.
** Staging late (after a materialisation) would replay a stale record OVER the
** newer base page.
*/
typedef struct BfGroup BfGroup;
struct BfGroup {
  u8         *aBuf;     /* batch scratch buffer, szPage bytes (owned) */
  int         szPage;   /* record-frame payload size */
  BfWalBatch  batch;    /* current open (unstaged) batch */
  int         nOpen;    /* records appended to the current batch */
  int         nTxn;     /* transactions merged into the current batch */
  u64         nStaged;  /* stats: batches handed to the WAL */
  u64         nDeferred;/* stats: commits that wrote nothing (grouped) */
};

typedef struct BfLogAllCtx BfLogAllCtx;
struct BfLogAllCtx {
  Pager      *pPager;   /* stage target (WAL) */
  BfGroup    *pGroup;   /* the cache's open batch */
  int         rc;       /* first error encountered, sticky */
};

/* The cache's open batch, allocated on first use.  NULL only on OOM. */
static BfGroup *bfGroupGet(BfCache *pBf, int szPage){
  BfGroup *g = (BfGroup*)pBf->pGroupCommit;
  if( g==0 ){
    if( szPage<=0 ) return 0;
    g = (BfGroup*)sqlite3_malloc(sizeof(*g));
    if( g==0 ) return 0;
    memset(g, 0, sizeof(*g));
    g->aBuf = (u8*)sqlite3_malloc(szPage);
    if( g->aBuf==0 ){ sqlite3_free(g); return 0; }
    g->szPage = szPage;
    sqlite3BfWalBatchInit(&g->batch, g->aBuf, g->szPage);
    pBf->pGroupCommit = (void*)g;
  }
  return g;
}

/* Finish and stage the open batch (if non-empty), then re-open it empty. */
static void bfGroupStage(BfGroup *g, Pager *pPager, int *pRc){
  if( *pRc!=SQLITE_OK || g->nOpen==0 ){
    g->nTxn = 0;
    return;
  }
  sqlite3BfWalBatchFinish(&g->batch);
  *pRc = sqlite3PagerBfStage(pPager, g->aBuf, g->szPage);
  g->nOpen = 0;
  g->nTxn = 0;
  g->nStaged++;
  sqlite3BfWalBatchInit(&g->batch, g->aBuf, g->szPage);
}

/* Append one record; on a full batch, stage it and retry into a fresh one. */
static void bfLogAppendRec(BfLogAllCtx *ctx, const BfWalRec *pRec){
  BfGroup *g = ctx->pGroup;
  int wrc;
  if( ctx->rc!=SQLITE_OK ) return;
  wrc = sqlite3BfWalBatchAppend(&g->batch, pRec);
  if( wrc==BFWAL_FULL ){
    bfGroupStage(g, ctx->pPager, &ctx->rc);          /* stage what we have */
    if( ctx->rc!=SQLITE_OK ) return;
    wrc = sqlite3BfWalBatchAppend(&g->batch, pRec);  /* retry into fresh batch */
  }
  if( wrc==BFWAL_OK ){
    g->nOpen++;
  }else{
    /* A single record that will not fit an empty page-sized batch, or NOMEM. */
    ctx->rc = (wrc==BFWAL_NOMEM) ? SQLITE_NOMEM_BKPT : SQLITE_CORRUPT_BKPT;
  }
}

/*
** Stage the cache's open record batch, if any, so its frames are written by the
** WAL commit of the transaction that is running RIGHT NOW.  Called before any
** base-page materialisation (see the ordering invariant above) and whenever a
** caller needs the group forced out.  No-op when nothing is pending.
*/
void sqlite3BfBtreeGroupStagePending(Btree *p){
  BfCache *pBf;
  BfGroup *g;
  int rc = SQLITE_OK;
  if( !p || !p->pBt ) return;
  pBf = sqlite3PagerGetBfCache(p->pBt->pPager);
  if( !pBf ) return;
  g = (BfGroup*)pBf->pGroupCommit;
  if( g==0 || g->nOpen==0 ) return;
  bfGroupStage(g, p->pBt->pPager, &rc);
  /* A staging failure here cannot fail the caller (flush paths return void),
  ** but it is not a correctness problem: the records are still DIRTY, so the
  ** materialisation about to run writes them to base pages instead. */
}

/*
** Release the open batch (cache teardown).  Any records still in it are dirty
** in the mini-pages, so the close-time flush has already materialised them.
*/
void sqlite3BfBtreeGroupFree(BfCache *pBf){
  BfGroup *g;
  if( !pBf || !pBf->pGroupCommit ) return;
  g = (BfGroup*)pBf->pGroupCommit;
  sqlite3_free(g->aBuf);
  sqlite3_free(g);
  pBf->pGroupCommit = 0;
}

/*
** Group-commit statistics for PRAGMA bf_cache_stats.
*/
void sqlite3BfBtreeGroupStats(Btree *p, u64 *pStaged, u64 *pDeferred,
                              int *pPending){
  BfCache *pBf = 0;
  BfGroup *g = 0;
  if( p && p->pBt ) pBf = btreeGetBfCache(p->pBt);
  if( pBf ) g = (BfGroup*)pBf->pGroupCommit;
  if( pStaged )   *pStaged   = g ? g->nStaged   : 0;
  if( pDeferred ) *pDeferred = g ? g->nDeferred : 0;
  if( pPending )  *pPending  = g ? g->nTxn      : 0;
}

static int bfLogAllCallback(void *pCtx, u32 pgno, BfMapEntry *pEntry){
  BfLogAllCtx *ctx = (BfLogAllCtx*)pCtx;
  BfMiniPage *pMini;
  int i, n;

  UNUSED_PARAMETER(pgno);
  if( ctx->rc!=SQLITE_OK ) return 0;
  if( pgno<=1 || pEntry->locType!=BF_LOC_MINI ) return 0;
  pMini = (BfMiniPage*)pEntry->pPage;
  if( !pMini ) return 0;

  n = sqlite3BfMiniPageCount(pMini);
  for(i=0; i<n; i++){
    const u8 *pKey, *pVal;
    int nKey, nVal;
    u8 op;
    BfWalRec rec;
    if( !sqlite3BfMiniPageDirtyUnloggedAt(pMini, i, &pKey,&nKey,&pVal,&nVal,&op) ){
      continue;
    }
    rec.pgno = pMini->ownerPgno;                     /* leaf pgno (Phase 1 key) */
    rec.op   = (op==BFOP_DELETE) ? BFWAL_OP_DELETE : BFWAL_OP_INSERT;
    rec.nKey = (u32)nKey;
    rec.pKey = pKey;
    rec.nVal = (op==BFOP_DELETE) ? 0 : (u32)nVal;
    rec.pVal = (op==BFOP_DELETE) ? 0 : pVal;
    bfLogAppendRec(ctx, &rec);
    if( ctx->rc!=SQLITE_OK ) return 0;               /* stop: flag stays set */
    sqlite3BfMiniPageMarkLoggedAt(pMini, i);         /* safe: see header note */
  }
  /* The loop ran to the end, so every dirty record on this page is now in a
  ** record batch.  Only here is it safe to clear the hint -- an early return
  ** above leaves it set, which keeps the page on the unlogged list and gets it
  ** retried on the next commit rather than silently dropped. */
  pMini->flags &= ~BF_MINI_F_UNLOGGED;
  return 0;
}

/*
** Gather every dirty-but-unlogged BF record across all mini-pages into WAL
** record-batch payloads and stage them for the pending commit.  Does NOT touch
** the base B-tree.  Returns SQLITE_OK, or an error (NOMEM/CORRUPT) in which case
** the caller MUST fail the commit so the rollback discards the logged marks.
**
** No-op (returns SQLITE_OK) when nothing is buffered.  Must only be called in
** WAL mode — in a non-WAL journal there is no record log to ride, so the caller
** uses sqlite3BfBtreeFlushAllDirty instead.
*/
int sqlite3BfBtreeLogAllDirty(Btree *p){
  BfCache *pBf;
  BfLogAllCtx ctx;
  BfGroup *g;
  int nGroup;

  if( !p || !p->pBt ) return SQLITE_OK;
  pBf = sqlite3PagerGetBfCache(p->pBt->pPager);
  if( !pBf ) return SQLITE_OK;
  if( !pBf->bDirtyInserts ) return SQLITE_OK;    /* nothing buffered to log */

  g = bfGroupGet(pBf, pBf->szPage);
  if( !g ) return SQLITE_NOMEM_BKPT;

  memset(&ctx, 0, sizeof(ctx));
  ctx.pPager = p->pBt->pPager;
  ctx.pGroup = g;
  ctx.rc     = SQLITE_OK;

  /* Three tiers, narrowest first.  The unlogged list holds what THIS
  ** transaction dirtied; the dirty list holds every mini-page not yet in its
  ** base page (which a one-row commit has no business walking -- it was 24.9%
  ** of cycles and quadratic over a run); the map walk is the last resort.
  ** Each tier falls through only when its list could not be allocated. */
  if( sqlite3BfUnlogListIterate(pBf, bfLogAllCallback, &ctx) ){
    if( sqlite3BfDirtyListIterate(pBf, bfLogAllCallback, &ctx) ){
      sqlite3BfMapIterate(pBf, bfLogAllCallback, &ctx);   /* both overflowed */
    }
  }
  if( ctx.rc!=SQLITE_OK ) return ctx.rc;

  /* Group commit: hold the batch open across up to nGroup transactions.  A
  ** deferred commit stages nothing, so sqlite3PagerCommitPhaseOne sees no
  ** staged payload and no dirty page and writes zero bytes for it. */
  nGroup = sqlite3BfGroupCommitTxns();
  g->nTxn++;
  if( nGroup<=1 || g->nTxn>=nGroup ){
    bfGroupStage(g, ctx.pPager, &ctx.rc);
  }else{
    g->nDeferred++;
  }

  /* bDirtyInserts stays set: the records are logged but still not in base, so
  ** pre-mutation flush + checkpoint/eviction flush must still run for them. */
  return ctx.rc;
}
#endif /* SQLITE_BF_INSERT_BUFFERING */

/*
** Context + callback to flush only the dirty leaves of one table (root).
*/
typedef struct BfFlushTableCtx BfFlushTableCtx;
struct BfFlushTableCtx {
  BfCache *pBf;
  Btree   *pBtree;
  Pgno     root;     /* table root whose leaves to flush */
  int      nFlushed;
};

static int bfFlushTableCallback(void *pCtx, u32 pgno, BfMapEntry *pEntry){
  BfFlushTableCtx *ctx = (BfFlushTableCtx*)pCtx;
  BfMiniPage *pMini;

  if( pgno<=1 || pEntry->locType!=BF_LOC_MINI ) return 0;
  pMini = (BfMiniPage*)pEntry->pPage;
  if( !pMini ) return 0;
  /* Cheap root test BEFORE the dirty scan: other tables' mini-pages are the
  ** common case in this iteration and rejecting them costs one compare. */
  if( pMini->rootPgno<=1 || (Pgno)pMini->rootPgno!=ctx->root ) return 0;
  if( !sqlite3BfMiniPageIsDirty(pMini) ) return 0;

  bfFlushOneMiniPage(ctx->pBf, ctx->pBtree, (Pgno)pMini->rootPgno, pMini);
  ctx->nFlushed++;
  return 0;
}

/*
** Flush every dirty mini-page belonging to the table rooted at pgnoRoot to the
** base B-tree, via bypass replay cursors.  Used as the per-leaf "fallback"
** survival mechanism: it is called before any base-tree mutation that could
** rebalance or free this table's leaves (so balance/freePage2 never observe a
** dirty mini-page outside a flush), and as the pre-scan flush so a scan sees
** all buffered rows.  Returns the number of mini-pages materialised.
*/
static int bfFlushTableDirty(BfCache *pBf, Btree *pBtree, Pgno pgnoRoot){
  BfFlushTableCtx ctx;
  if( !pBf || !pBtree || pgnoRoot<=1 ) return 0;
  if( pBf->bBypassActive || pBf->bMergingActive ) return 0;
  /* Nothing buffered anywhere ⇒ nothing to materialise.  Skips a full map walk
  ** on every mutation of a table that has no dirty records. */
  if( !pBf->bDirtyInserts ) return 0;
  /* No write transaction ⇒ nothing can be materialised (see bfFlushOneMiniPage).
  ** Must report 0: callers treat a non-zero return as "the tree changed, redo
  ** the descent", which would spin forever against an unchanged tree. */
  if( pBtree->inTrans!=TRANS_WRITE ) return 0;
  /* Nothing buffered since the last commit/rollback ⇒ no dirty mini-page can
  ** exist; skip the O(map) walk.  Conservative: never 0 while dirty. */
  if( !pBf->bDirtyInserts ) return 0;
  ctx.pBf = pBf;
  ctx.pBtree = pBtree;
  ctx.root = pgnoRoot;
  ctx.nFlushed = 0;
  pBf->bMergingActive = 1;
  if( sqlite3BfDirtyListIterate(pBf, bfFlushTableCallback, &ctx) ){
    sqlite3BfMapIterate(pBf, bfFlushTableCallback, &ctx); /* list overflowed */
  }
  pBf->bMergingActive = 0;
  return ctx.nFlushed;
}

/*
** Pre-mutation flush hook (Phase 1 per-leaf survival, conservative form).
** Called from sqlite3BtreeInsert / sqlite3BtreeDelete just before a base-tree
** modification that may rebalance or free this table's leaves.  It writes out
** every buffered (dirty) record of the cursor's table so that the subsequent
** balance_nonroot / freePage2 only ever see CLEAN mini-pages (which they may
** safely drop).  Returns non-zero if anything was flushed, so the caller can
** re-seek its cursor (the flush replay invalidates open cursors).
**
** btreeGetBfCache() returns NULL while a flush is already in progress
** (bBypassActive), so this is automatically a no-op during replay — replayed
** inserts/deletes hit the base tree directly and never recurse here.
*/
int sqlite3BfBtreeFlushTableForMutation(BtCursor *pCur){
  BfCache *pBf;
  if( !pCur || !pCur->pBt || !pCur->curIntKey ) return 0;
  if( pCur->pgnoRoot<=1 ) return 0;
  pBf = btreeGetBfCache(pCur->pBt);
  if( !pBf ) return 0;
  return bfFlushTableDirty(pBf, pCur->pBtree, pCur->pgnoRoot);
}

/* -------------------------------------------------------------------------
** Map-clear for ROLLBACK: discard all BF state for a B-tree
** ----------------------------------------------------------------------- */

static int bfClearOneEntry(void *pCtx, u32 pgno, BfMapEntry *pEntry){
  UNUSED_PARAMETER(pCtx);
  UNUSED_PARAMETER(pgno);
  /* Don't bother calling BfCircularBufferDealloc — the FIFO eviction
  ** will reclaim memory naturally.  Just unlink the pointer. */
  pEntry->locType = BF_LOC_NULL;
  pEntry->pPage   = 0;
  return 0;
}

/*
** Clear all BF mini-page state for a B-tree (called on ROLLBACK).
** Stale BFOP_INSERT records from rolled-back transactions must not
** survive; discarding the entire cache for safety is correct.
*/
/*
** Apply a new circular-buffer capacity to a cache that already exists.
**
** PRAGMA bf_cache_size only wrote the global that bfCacheCreate() reads, and
** the cache is built the first time the pager needs a page -- during
** PRAGMA cache_size / journal_mode / the schema load, i.e. ALWAYS before the
** BF pragmas can run.  (They must run after journal_mode, which reopens the
** pager and drops BF settings made earlier, so there is no ordering that
** worked.)  The configured size was therefore never in force: every cache ran
** at the 8 MiB BF_DEFAULT_BUFFER_SIZE while PRAGMA bf_cache_stats faithfully
** reported the 256 MiB that had been asked for.  Measured cost of that on a
** skewed read workload: 34% record hit rate instead of 87%, 2k cached records
** instead of 628k, and 1.2M evictions instead of none.
**
** Resizing means discarding the ring, so every mini-page in it goes too.  That
** is safe only for CLEAN content: BFOP_CACHE/BFOP_PHANTOM records merely
** duplicate the base page, so dropping them costs at most a re-read.  If the
** cache holds buffered writes the base tree does not have yet, refuse and keep
** the current ring -- a smaller-than-requested cache is a performance problem,
** losing a committed row is a correctness one.
**
** Returns SQLITE_OK if the capacity is now in force (including when it already
** was), SQLITE_BUSY if dirty content made the resize unsafe.
*/
int sqlite3BfBtreeResizeCache(Btree *p, u64 newCapacity){
  BfCache *pBf;

  if( !p || !p->pBt ) return SQLITE_OK;
  pBf = sqlite3PagerGetBfCache(p->pBt->pPager);
  if( !pBf ) return SQLITE_OK;   /* not built yet: creation will read the global */
  if( pBf->cb.capacity == newCapacity ) return SQLITE_OK;
  if( pBf->bDirtyInserts || pBf->nDirty ) return SQLITE_BUSY;

  /* Drop every mapping first: the mini-pages they point at live inside the
  ** ring we are about to free. */
  sqlite3BfMapIterate(pBf, bfClearOneEntry, NULL);
  pBf->bDirtyInserts = 0;
  sqlite3BfUnlogListReset(pBf);       /* same reason as the mappings above */
  sqlite3BfCircularBufferDestroy(&pBf->cb);
  return sqlite3BfCircularBufferInit(&pBf->cb, newCapacity);
}

void sqlite3BfBtreeClearCache(Btree *p){
  BfCache *pBf;
  if( !p || !p->pBt ) return;
  pBf = sqlite3PagerGetBfCache(p->pBt->pPager);
  if( !pBf ) return;
  sqlite3BfMapIterate(pBf, bfClearOneEntry, NULL);
  pBf->bDirtyInserts = 0;  /* rolled-back inserts discarded */
  sqlite3BfUnlogListReset(pBf);   /* the pages it named no longer exist */
}

/* -------------------------------------------------------------------------
** Public btree hooks
** ----------------------------------------------------------------------- */

/*
** Fetch payload for a point read from the BF mini-page cache.
**
** Mini-pages are keyed by pgnoRoot so this works whether the cursor is
** at the root (after BF short-circuited a moveto) or at a leaf page.
*/
int sqlite3BfBtreeFetchPayload(
  BtCursor *pCur,
  u32 offset,
  u32 amt,
  void *pBuf
){
  BtShared *pBt;
  BfCache  *pBf;
  i64  rowid;
  int  nBuf, nReq;
  void *pOut = pBuf;
  void *pTmp = 0;
  int  rc;

  if( !pCur || !pCur->pBt ) return SQLITE_NOTFOUND;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return SQLITE_NOTFOUND;
  if( amt==0 ) return SQLITE_OK;
  if( !pCur->curIntKey ) return SQLITE_NOTFOUND;  /* index tables: unsupported */

  /* Key by the leaf the cursor sits on; refuse (read from base) otherwise. */
  u32 leaf = bfCursorLeafPgno(pCur);
  if( leaf==0 ) return SQLITE_NOTFOUND;

  rowid = pCur->info.nKey;

  {
    u64 nReq64 = (u64)offset + (u64)amt;
    if( nReq64>(u64)0x7fffffff ) return SQLITE_NOTFOUND;
    nReq = (int)nReq64;
  }

  if( offset>0 ){
    pTmp = sqlite3_malloc64((u64)nReq);
    if( !pTmp ){
      BF_BT_TRACE("payload-alloc-failed", NULL, nReq);
      return SQLITE_NOMEM_BKPT;
    }
    pOut = pTmp;
  }
  nBuf = nReq;

  {
    u8 keyBuf[8];
    bfEncodeRowid(rowid, keyBuf);
    rc = sqlite3BfRecordRead(pBf, leaf, keyBuf, 8, pOut, &nBuf);
  }

  if( rc==BF_OK ){
    if( offset>0 ){
      if( nBuf<nReq ){
        sqlite3_free(pTmp);
        pBf->nMiniPageMiss++;
        return SQLITE_NOTFOUND;
      }
      memcpy(pBuf, &((u8*)pOut)[offset], amt);
      sqlite3_free(pTmp);
    }
    pBf->nMiniPageHit++;
    return SQLITE_OK;
  }

  if( pTmp ) sqlite3_free(pTmp);
  pBf->nMiniPageMiss++;
  return SQLITE_NOTFOUND;
}

/*
** Buffer a cell insert into the BF mini-page.
** Called when BF write-buffering wants to skip the base page entirely.
** Only used for rowid tables with small payloads.
*/
/*
** Remember the largest rowid buffered for table `root`.  Feeds
** sqlite3BfBtreeMaxBufferedRowid so OP_NewRowid does not have to materialise
** the table just to learn the maximum key -- which, before this, made every
** append flush the whole table (46.7% of an append profile).
*/
static void bfNoteMaxRowid(BfCache *pBf, u32 root, const void *pKey){
  const u8 *p = (const u8*)pKey;
  i64 rowid = 0;
  int i;
  if( root<=1 || pBf->bMaxRowidUnknown ) return;
  for(i=0; i<8; i++){ rowid = (rowid<<8) | p[i]; }
  for(i=0; i<pBf->nMaxRowid; i++){
    if( pBf->aMaxRoot[i]==root ){
      if( rowid > pBf->aMaxRowid[i] ) pBf->aMaxRowid[i] = rowid;
      return;
    }
  }
  if( pBf->nMaxRowid >= BF_MAXROWID_SLOTS ){
    pBf->bMaxRowidUnknown = 1;    /* too many tables: revert to flushing */
    return;
  }
  pBf->aMaxRoot[pBf->nMaxRowid] = root;
  pBf->aMaxRowid[pBf->nMaxRowid] = rowid;
  pBf->nMaxRowid++;
}

/*
** Largest rowid this cache has buffered for the cursor's table.  Returns 1 and
** sets *pMax when the answer is known (0 tracked rows counts as "not known",
** so the caller keeps its base-tree answer).
*/
int sqlite3BfBtreeMaxBufferedRowid(BtCursor *pCur, i64 *pMax){
  BfCache *pBf;
  int i;
#ifdef SQLITE_BF_NO_MAXROWID
  return 0;                      /* ablation: force the old flush-then-look */
#endif
  if( !pCur || !pCur->pBt || pCur->pgnoRoot<=1 ) return 0;
  pBf = btreeGetBfCache(pCur->pBt);
  if( !pBf || pBf->bMaxRowidUnknown ) return 0;
  for(i=0; i<pBf->nMaxRowid; i++){
    if( pBf->aMaxRoot[i]==pCur->pgnoRoot ){
      *pMax = pBf->aMaxRowid[i];
      return 1;
    }
  }
  return 0;
}

int sqlite3BfBtreeInsertCell(
  BtCursor *pCur,
  const void *pKey,
  int nKey,
  const void *pData,
  int nData
){
  BtShared *pBt;
  BfCache  *pBf;
  int rc;

  if( !pCur || !pCur->pBt ) return SQLITE_NOTFOUND;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return SQLITE_NOTFOUND;

  if( nData>BF_MAX_MINI_PAGE || nKey!=8 ){ pBf->nInsertRefused++; return SQLITE_FULL; }

  /* Per-leaf keying (Phase 1): buffer the insert into the mini-page of the
  ** leaf the cursor has descended to, not the table root.  If the cursor is
  ** not parked on a real leaf, refuse so the caller does a base write. */
  {
    u32 leaf = bfCursorLeafPgno(pCur);
    if( leaf==0 ){ pBf->nInsertRefused++; return SQLITE_FULL; }
    rc = sqlite3BfRecordWrite(pBf, leaf, pKey, nKey, pData, nData, BFOP_INSERT);
    if( rc==BF_OK ){
      bfTagLeafRoot(pBf, leaf, pCur->pgnoRoot);
      pBf->bDirtyInserts = 1;  /* arm the pre-mutation/pre-scan flush */
      pBf->nBufferedInserts++;
      bfNoteMaxRowid(pBf, pCur->pgnoRoot, pKey);
      return SQLITE_OK;
    }
  }
  if( rc==SQLITE_NOMEM ) return SQLITE_NOMEM;
  pBf->nInsertRefused++;

  /* The mini-page is full and could not be upgraded (at max size class), or
  ** the circular buffer is out of memory.  We must NOT evict this table's
  ** dirty mini-page to make room: that would discard the buffered inserts
  ** already accumulated for this transaction (eviction marks records clean
  ** without applying them).  Instead, fall through to the normal base-page
  ** write for this record.  The records already buffered stay dirty and are
  ** applied to the base table by the commit-time flush. */
  return SQLITE_FULL;  /* caller falls through to base-page write */
}

/*
** Invalidate the per-leaf read cache for a row being deleted, by writing a
** negative-cache entry on the leaf the delete descended to.  The base-page
** delete still happens after this (deletes are write-through in Phase 0/1), so
** the entry is a CLEAN BFOP_PHANTOM, not a dirty tombstone: it only suppresses
** a stale BFOP_CACHE for this rowid on the same leaf and never needs flushing.
** (Write-back deletes — dirty leaf tombstones — return in Phase 2.)
*/
int sqlite3BfBtreeDeleteCell(
  BtCursor *pCur,
  const void *pKey,
  int nKey
){
  BtShared *pBt;
  BfCache  *pBf;
  u8        keyBuf[8];
  u32       leaf;

  /* Best-effort cache invalidation: "no BF cache to touch" is success, not an
  ** error.  sqlite3BtreeDelete propagates any rc other than OK/FULL, so the
  ** sibling hooks (CacheRecord/Promote/Phantom) all return OK here too;
  ** returning SQLITE_NOTFOUND would abort the base delete whenever BF is
  ** unavailable — globally off (PRAGMA bf_cache=OFF), during a bypass flush
  ** replay, or disabled for an auto-vacuum DB (btreeUsesBfCache). */
  if( !pCur || !pCur->pBt ) return SQLITE_OK;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return SQLITE_OK;

  /* Key by the leaf holding the row.  If the cursor is not on a leaf there is
  ** no cache entry to invalidate here; the base delete is authoritative. */
  leaf = bfCursorLeafPgno(pCur);
  if( leaf==0 ) return SQLITE_OK;

  /* Canonicalise rowid keys to the same 8-byte big-endian encoding used by
  ** the read paths.  The caller passes the raw i64 bytes (native endianness);
  ** without this, the entry would be stored under a key the big-endian lookups
  ** can never match, leaving a stale cached row visible. */
  if( nKey==(int)sizeof(i64) ){
    i64 rowid;
    memcpy(&rowid, pKey, sizeof(i64));
    bfEncodeRowid(rowid, keyBuf);
    pKey = keyBuf;
    nKey = 8;
  }

  /* A PHANTOM is an in-place same-or-smaller overwrite of any existing cache
  ** record for this key, so it cannot fail for space; ignore the result. */
  (void)sqlite3BfRecordWrite(pBf, leaf, pKey, nKey, "", 0, BFOP_PHANTOM);
  bfTagLeafRoot(pBf, leaf, pCur->pgnoRoot);
  return SQLITE_OK;
}

#if defined(SQLITE_BF_INSERT_BUFFERING) && !defined(SQLITE_BF_NO_WRITEBACK_DELETE)
/*
** Stage 2.3 — write-back delete.  Buffer a dirty BFOP_DELETE tombstone for the
** rowid `pKey` (raw i64 bytes) on the leaf the delete cursor is positioned on,
** leaving the base cell in place to be removed lazily at flush/commit.  The
** tombstone is honoured by point reads (moveto_table_finish, bf_status==2),
** the descent shortcut (BF_DELETED ⇒ no shortcut ⇒ full descent reports absent)
** and forward merge scans (bfMergePick suppresses the matching base cell); the
** flush replay applies it through bfApplyOneRecord (moveto+delete, with
** "key not found in base" treated as success — the row may have existed only as
** a buffered insert the tombstone cancels in-place inside the mini-page).
**
** Returns SQLITE_OK if buffered (caller skips the base delete), SQLITE_FULL if
** the mini-page is full at its max size class (caller falls through to a base
** delete), or SQLITE_NOMEM.
*/
int sqlite3BfBtreeBufferDelete(BtCursor *pCur, const void *pKey, int nKey){
  BtShared *pBt;
  BfCache  *pBf;
  u8        keyBuf[8];
  u32       leaf;
  int       rc;

  if( !pCur || !pCur->pBt ) return SQLITE_FULL;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return SQLITE_FULL;

  /* Key by the leaf the delete descended to; if the cursor is not on a real
  ** leaf there is nowhere to home the tombstone — refuse, base delete proceeds. */
  leaf = bfCursorLeafPgno(pCur);
  if( leaf==0 ) return SQLITE_FULL;

  /* Canonical 8-byte big-endian rowid key (same encoding as the read paths). */
  if( nKey==(int)sizeof(i64) ){
    i64 rowid;
    memcpy(&rowid, pKey, sizeof(i64));
    bfEncodeRowid(rowid, keyBuf);
    pKey = keyBuf;
    nKey = 8;
  }

  rc = sqlite3BfRecordWrite(pBf, leaf, pKey, nKey, "", 0, BFOP_DELETE);
  if( rc==BF_OK ){
    bfTagLeafRoot(pBf, leaf, pCur->pgnoRoot);
    pBf->bDirtyInserts = 1;   /* arm the pre-mutation/pre-scan flush */
    pBf->nWriteBackDeletes++;
    return SQLITE_OK;
  }
  if( rc==SQLITE_NOMEM ) return SQLITE_NOMEM;
  return SQLITE_FULL;        /* mini-page full: caller does a base delete */
}

/*
** Stage 2.3 — forward merge-scan tombstone test.  Returns 1 iff rowid `rowid`
** has a dirty BFOP_DELETE tombstone on leaf `leaf`'s mini-page, so the merge
** scan must suppress the matching base cell.  A clean BFOP_PHANTOM (key the
** base never held) does NOT suppress; only a write-back delete does.
*/
int sqlite3BfBtreeKeyTombstoned(BtCursor *pCur, Pgno leaf, i64 rowid){
  BtShared   *pBt;
  BfCache    *pBf;
  BfMapEntry *pEntry;
  BfMiniPage *pMini;
  u8          keyBuf[8], op = 0;

  if( !pCur || !pCur->pBt || leaf<=1 ) return 0;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return 0;
  pEntry = sqlite3BfMapLookup(pBf, leaf);
  if( !pEntry || pEntry->locType!=BF_LOC_MINI || !pEntry->pPage ) return 0;
  pMini = (BfMiniPage*)pEntry->pPage;
  bfEncodeRowid(rowid, keyBuf);
  if( !sqlite3BfMiniPageLookupOp(pMini, keyBuf, 8, &op) ) return 0;
  return op==BFOP_DELETE;
}
#endif /* SQLITE_BF_INSERT_BUFFERING && !SQLITE_BF_NO_WRITEBACK_DELETE */

/*
** Stage 2.1 — scan-mode gate.  Decide whether the upcoming scan on pCur may
** use merge-iteration (Stage 2.2), in which case the wholesale pre-scan flush
** is skipped and btreeNext/Previous merge each leaf's mini-page with its base
** cells on the fly, honouring buffered inserts and tombstones.  When merge-
** iteration is NOT available the scan must fall back to the Phase 1 behaviour:
** flush every dirty mini-page of the table up front so a plain base-tree walk
** sees all rows.
**
** Stage 2.2 (forward INSERT-injection merge) is compiled only with
** SQLITE_BF_INSERT_BUFFERING — it is the only configuration in which dirty
** records (buffered BFOP_INSERT) exist at all.  In every other build there is
** nothing to merge, so the gate returns 0 and the scan flushes (a no-op when
** nothing is buffered), preserving Phase 1 behaviour byte-for-byte.
**
** When eligible, merge mode requires: an intkey (rowid) table cursor on a real
** table (pgnoRoot>1), a non-write cursor (write cursors get saved/restored from
** unsafe points, so they keep the pre-flush), and at least one buffered insert
** outstanding (pBf->bDirtyInserts).  Reverse iteration, COUNT and LAST keep the
** pre-flush (see their call sites); only forward Next/First merge.
*/
#if defined(SQLITE_BF_INSERT_BUFFERING)
static int bfScanCanMerge(BtCursor *pCur){
  BfCache *pBf;
  if( !pCur || !pCur->pBt ) return 0;
  /* curIntKey is only latched by moveToRoot (btree.c), so it is still 0 on a
  ** cursor that has not descended yet — which is exactly the state
  ** sqlite3BtreeFirst arms merge in.  pKeyInfo==0 is the static "this is a
  ** rowid table cursor" fact, known from cursor creation, so use it as the
  ** pre-descent form of the same test (without it merge never armed on a fresh
  ** cursor and every scan fell back to the flush path). */
  if( (!pCur->curIntKey && pCur->pKeyInfo!=0) || pCur->pgnoRoot<=1 ) return 0;
  if( (pCur->curFlags & BTCF_WriteFlag)!=0 ) return 0;
  pBf = btreeGetBfCache(pCur->pBt);
  if( !pBf || !pBf->bDirtyInserts ) return 0;
  return 1;
}
#endif

/*
** Begin a forward merge scan (Stage 2.2).  Called only from sqlite3BtreeFirst
** (full-table forward scans).  If the cursor is eligible — buffering build,
** intkey table, non-write cursor, and buffered inserts outstanding — arm merge
** mode on the cursor and return 1; the caller then SKIPS the pre-scan flush and
** lets btreeNext merge each leaf's buffered inserts with its base cells.  When
** ineligible returns 0 and the caller flushes as in Phase 1.
**
** Range seeks (TableMovetoForScan), LAST and COUNT keep the pre-scan flush:
** they position via TableMoveto, which has no merge awareness, so they must see
** materialised rows.  Only First/Next merge.
*/
int sqlite3BfBtreeBeginMergeScan(BtCursor *pCur){
#if defined(SQLITE_BF_INSERT_BUFFERING) && !defined(SQLITE_BF_NO_MERGE_SCAN)
  BfCache *pBf;
  if( !bfScanCanMerge(pCur) ) return 0;
  pCur->bfMerge = 1;
  pCur->bfMergeRev = 0;
  pCur->bfOnMini = 0;
  pCur->bfBaseDone = 0;
  pCur->bfMergeLeaf = 0;
  pCur->bfIx = 0;
  pBf = btreeGetBfCache(pCur->pBt);
  if( pBf ) pBf->nMergeScans++;
  return 1;
#else
  (void)pCur;
  return 0;
#endif
}

/*
** Begin a REVERSE merge scan (Stage 2.4).  Called only from sqlite3BtreeLast
** (full-table backward scans).  Same eligibility test as the forward arm; the
** cursor is additionally marked bfMergeRev so btreePrevious steps through
** bfMergePickPrev and a stray forward step bails instead of mis-merging.
*/
int sqlite3BfBtreeBeginMergeScanRev(BtCursor *pCur){
#if defined(SQLITE_BF_INSERT_BUFFERING) && !defined(SQLITE_BF_NO_MERGE_SCAN)
  BfCache *pBf;
  if( !bfScanCanMerge(pCur) ) return 0;
  pCur->bfMerge = 1;
  pCur->bfMergeRev = 1;
  pCur->bfOnMini = 0;
  pCur->bfBaseDone = 0;
  pCur->bfMergeLeaf = 0;
  pCur->bfIx = 0;
  pBf = btreeGetBfCache(pCur->pBt);
  if( pBf ) pBf->nMergeScans++;
  return 1;
#else
  (void)pCur;
  return 0;
#endif
}

/*
** Merge eligibility predicate with no side effects — used by sqlite3BtreeCount
** to decide between the (merge-blind) page-walk count and a merged First/Next
** scan.  Returns 0 in every non-buffering build.
*/
int sqlite3BfBtreeCanMergeScan(BtCursor *pCur){
#if defined(SQLITE_BF_INSERT_BUFFERING) && !defined(SQLITE_BF_NO_MERGE_SCAN)
  return bfScanCanMerge(pCur);
#else
  (void)pCur;
  return 0;
#endif
}

/*
** Pre-scan flush: materialise any BFOP_INSERT records for the table about
** to be scanned into the base B-tree pages, so the scan sees all rows.
**
** Called from sqlite3BtreeFirst (before AND after moveToRoot), sqlite3BtreeNext,
** and sqlite3BtreeCount. The cursor eState may be CURSOR_INVALID when called
** before moveToRoot — this is intentional: the base B-tree may be completely
** empty while the BF mini-page holds buffered inserts that haven't been applied.
** The bMergingActive guard prevents re-entrant calls.
**
** Forward full scans skip this flush (sqlite3BtreeFirst calls
** sqlite3BfBtreeBeginMergeScan instead and merge-iterates); every other scan
** entry point (LAST, COUNT, range-seek positioning) still flushes here.
**
** Returns 1 if a dirty flush was performed (caller should re-root the cursor),
** 0 otherwise.
*/
int sqlite3BfBtreePrepareForScan(BtCursor *pCur){
  BtShared *pBt;
  BfCache  *pBf;

  /* pBt and pBtree must be set (they are at cursor creation time).
  ** eState is NOT required to be CURSOR_VALID — callers invoke this
  ** before the first moveToRoot() to handle the empty-base-btree case. */
  if( !pCur || !pCur->pBt ) return 0;

  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return 0;
  if( pBf->bMergingActive ) return 0;

  /* Flush ALL dirty mini-pages of this table before the scan.  Mini-pages are
  ** keyed by leaf (Phase 1); a table's dirty rows can be spread across many
  ** leaves.  Forward full scans skip this via sqlite3BfBtreeBeginMergeScan and
  ** merge-iterate instead (Stage 2.2); range seeks, LAST and COUNT keep it.
  ** Safe with any cursor state — saveAllCursors is a no-op for CURSOR_INVALID. */
  return bfFlushTableDirty(pBf, pCur->pBtree, pCur->pgnoRoot)>0 ? 1 : 0;
}

#if defined(SQLITE_BF_INSERT_BUFFERING)
/*
** Merge-scan primitive (Stage 2.2).  Starting at sorted index *pIx in the
** mini-page of leaf `leaf`, find the next buffered BFOP_INSERT record.  On
** success: copies the record value into pBuf (capacity nCap), writes its rowid
** to *pRowid and value length to *pnVal, sets *pIx to the index AT which the
** insert was found (the caller advances past it only when it actually emits
** the record), and returns 1.  Returns 0 when no further INSERT records exist
** (and sets *pIx to the record count).  Returns -1 if the leaf's mini-page has
** vanished/changed or a value does not fit pBuf — the caller must then bail to
** the flush-restart fallback.
**
** Clean records (BFOP_CACHE/BFOP_PHANTOM) are skipped: a CACHE duplicates an
** existing base cell and a PHANTOM marks a confirmed-absent key, so neither
** contributes a row the base scan does not already produce (or correctly
** omit).  The map is re-looked-up on every call — a mini-page pointer is never
** cached across cursor steps because eviction/rebalance can free it.
*/
int sqlite3BfBtreeMergeNextInsert(
  BtCursor *pCur, Pgno leaf, int *pIx, i64 *pRowid, void *pBuf, int nCap,
  int *pnVal
){
  BtShared   *pBt;
  BfCache    *pBf;
  BfMapEntry *pEntry;
  BfMiniPage *pMini;
  int ix, n;

  if( !pCur || !pCur->pBt ) return -1;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return -1;
  pEntry = sqlite3BfMapLookup(pBf, leaf);
  if( !pEntry || pEntry->locType!=BF_LOC_MINI || !pEntry->pPage ){
    *pIx = 0;
    return 0;                 /* no mini-page on this leaf ⇒ no inserts */
  }
  pMini = (BfMiniPage*)pEntry->pPage;
  n = sqlite3BfMiniPageCount(pMini);
  for(ix=*pIx; ix<n; ix++){
    const u8 *pKey, *pVal;
    int nKey, nVal, k;
    i64 r = 0;
    u8 op;
    if( !sqlite3BfMiniPageAt(pMini, ix, &pKey, &nKey, &pVal, &nVal, &op) ){
      return -1;
    }
    if( op!=BFOP_INSERT ) continue;       /* skip clean cache/phantom records */
    if( nKey!=8 || nVal>nCap ) return -1; /* unexpected ⇒ bail */
    for(k=0; k<8; k++){ r = (r<<8) | pKey[k]; }
    *pRowid = r;
    if( nVal>0 ) memcpy(pBuf, pVal, nVal);
    *pnVal = nVal;
    *pIx = ix;                            /* report the found index */
    pBf->nMergeInserts++;
    return 1;
  }
  *pIx = n;
  return 0;
}

/*
** Merge-scan primitive (Stage 2.4) — the reverse twin of
** sqlite3BfBtreeMergeNextInsert.  Starting at sorted index *pIx in the
** mini-page of leaf `leaf` and walking DOWN, find the previous buffered
** BFOP_INSERT record.  *pIx may be any int on entry: it is clamped to the last
** record index, so a caller starting at the leaf's end passes INT_MAX; a
** negative *pIx means "already exhausted" and returns 0 immediately.
**
** On success: copies the value into pBuf, writes the rowid/value length, sets
** *pIx to the index AT which the insert was found, returns 1.  Returns 0 when
** no earlier INSERT exists (and sets *pIx to -1).  Returns -1 on the same bail
** conditions as the forward routine (mini-page gone/changed, value too big).
*/
int sqlite3BfBtreeMergePrevInsert(
  BtCursor *pCur, Pgno leaf, int *pIx, i64 *pRowid, void *pBuf, int nCap,
  int *pnVal
){
  BtShared   *pBt;
  BfCache    *pBf;
  BfMapEntry *pEntry;
  BfMiniPage *pMini;
  int ix, n;

  if( !pCur || !pCur->pBt ) return -1;
  if( *pIx<0 ) return 0;                /* exhausted below index 0 */
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return -1;
  pEntry = sqlite3BfMapLookup(pBf, leaf);
  if( !pEntry || pEntry->locType!=BF_LOC_MINI || !pEntry->pPage ){
    *pIx = -1;
    return 0;                 /* no mini-page on this leaf ⇒ no inserts */
  }
  pMini = (BfMiniPage*)pEntry->pPage;
  n = sqlite3BfMiniPageCount(pMini);
  ix = *pIx;
  if( ix>n-1 ) ix = n-1;                /* clamp the "start at the end" entry */
  for(; ix>=0; ix--){
    const u8 *pKey, *pVal;
    int nKey, nVal, k;
    i64 r = 0;
    u8 op;
    if( !sqlite3BfMiniPageAt(pMini, ix, &pKey, &nKey, &pVal, &nVal, &op) ){
      return -1;
    }
    if( op!=BFOP_INSERT ) continue;       /* skip clean cache/phantom records */
    if( nKey!=8 || nVal>nCap ) return -1; /* unexpected ⇒ bail */
    for(k=0; k<8; k++){ r = (r<<8) | pKey[k]; }
    *pRowid = r;
    if( nVal>0 ) memcpy(pBuf, pVal, nVal);
    *pnVal = nVal;
    *pIx = ix;                            /* report the found index */
    pBf->nMergeInserts++;
    return 1;
  }
  *pIx = -1;
  return 0;
}

/*
** Merge-scan bail (Stage 2.2).  Flush every dirty mini-page of the cursor's
** table to the base tree and clear the cursor's merge state, so the scan can
** continue as a plain base-tree walk (all buffered rows are now real cells).
** Called from the safe restore point (btreeRestoreCursorPosition) when a merge
** cursor was saved, and on the rare in-step bail.  Returns 1 if a flush
** occurred, 0 otherwise.
*/
int sqlite3BfBtreeMergeBail(BtCursor *pCur){
  BtShared *pBt;
  BfCache  *pBf;
  int wasMerge;
  int n = 0;
  if( !pCur ) return 0;
  wasMerge = pCur->bfMerge;
  pCur->bfMerge = 0;
  pCur->bfMergeRev = 0;
  pCur->bfOnMini = 0;
  pCur->bfBaseDone = 0;
  pCur->bfMergeLeaf = 0;
  pCur->bfIx = 0;
  if( !pCur->pBt ) return 0;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return 0;
  if( wasMerge ) pBf->nMergeBail++;     /* count merge scans that fell back */
  n = bfFlushTableDirty(pBf, pCur->pBtree, pCur->pgnoRoot);
  return n>0 ? 1 : 0;
}
#endif /* SQLITE_BF_INSERT_BUFFERING */

/*
** Check existence of a record in the BF cache for rowid tables.
** Returns: 1=found(INSERT/CACHE), 2=tombstoned(DELETE/PHANTOM), 0=not in BF
*/
int sqlite3BfBtreeRecordExists(
  BtCursor *pCur,
  const void *pKey,
  int nKey
){
  BtShared *pBt;
  BfCache  *pBf;
  u8        keyBuf[8];
  int       nBuf = 0, rc;
  u32       leaf;

  if( !pCur || !pCur->pBt ) return -1;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return -1;

  /* Key by the leaf the cursor descended to; if it is not on a leaf, report
  ** "not cached" so the base page stays authoritative. */
  leaf = bfCursorLeafPgno(pCur);
  if( leaf==0 ) return 0;

  /* Rowid is passed as raw bytes; re-encode canonically. */
  if( nKey==sizeof(i64) ){
    i64 rowid;
    memcpy(&rowid, pKey, sizeof(i64));
    bfEncodeRowid(rowid, keyBuf);
    pKey = keyBuf;
    nKey = 8;
  }

  rc = sqlite3BfRecordRead(pBf, leaf, pKey, nKey, 0, &nBuf);
  if( rc==BF_OK ){     pBf->nMiniPageHit++;  return 1; }
  if( rc==BF_DELETED ){ pBf->nMiniPageHit++; return 2; }
  pBf->nMiniPageMiss++;
  return 0;
}

/*
** Descent shortcut (Stage 1.6).  During a table descent, just before the
** cursor would read the leaf page `chldPg`, ask whether that leaf holds a
** CLEAN cached copy of the row for `intKey`.  If so the caller can serve the
** row directly from the BF mini-page and skip the (possibly cold) leaf read.
**
** Returns 1 ONLY for a clean BFOP_CACHE hit.  A dirty buffered insert, a
** tombstone/phantom (BF_DELETED), or a miss all return 0 so the caller falls
** through to the normal full descent (and its existing dirty-flush logic).
**
** Soundness: the map only ever stores mini-pages under leaf pgnos, and a
** leaf's mini-page is forgotten when the page is freed (freePage2->ForgetPage,
** auto-vacuum disabled), so a BF_LOC_MINI entry for chldPg means chldPg is the
** current correct leaf for intKey and the cached bytes are authoritative.
** Interior pgnos are never in the map, so this only ever fires at the true
** leaf edge.
*/
int sqlite3BfBtreeDescentServe(BtCursor *pCur, Pgno chldPg, i64 intKey,
                               void *pBuf, int *pnBuf){
  BtShared   *pBt;
  BfCache    *pBf;
  BfMapEntry *pEntry;
  BfMiniPage *pMini;
  u8          keyBuf[8];
  int         rc;

  if( !pCur || !pCur->pBt || chldPg<=1 || !pBuf || !pnBuf || *pnBuf<=0 ) return 0;
  if( !pCur->curIntKey ) return 0;              /* rowid tables only */
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return 0;
  /* Never shortcut while a bypass/merge replay is mutating the base tree, while
  ** suppressed (range-seek / materialise re-descent), or for a write cursor --
  ** writes need the physical leaf and must not be left in a leaf-less state. */
  if( pBf->bBypassActive || pBf->bMergingActive || pBf->bShortcutSuppressed ){
    return 0;
  }
  if( (pCur->curFlags & BTCF_WriteFlag)!=0 ) return 0;

  pEntry = sqlite3BfMapLookup(pBf, chldPg);
  if( !pEntry || pEntry->locType!=BF_LOC_MINI || !pEntry->pPage ) return 0;
  pMini = (BfMiniPage*)pEntry->pPage;

  /* ONE search, and it copies.  This used to be a probe that searched without a
  ** buffer followed by a serve that searched again with one -- three map
  ** lookups and two binary searches per served read, and two nMiniPageHit
  ** increments for one hit, which inflated the reported record hit rate. */
  bfEncodeRowid(intKey, keyBuf);
  rc = sqlite3BfMiniPageSearch(pMini, keyBuf, 8, pBuf, pnBuf);
  if( rc!=BF_OK ){
    /* BF_DELETED (tombstone/phantom) or miss: let the real descent decide. */
    if( rc==BF_DELETED ) pBf->nMiniPageHit++;
    else pBf->nMiniPageMiss++;
    return 0;
  }
  pBf->nMiniPageHit++;
  return 1;
}


/*
** Length of the clean cached record the descent shortcut is about to serve,
** for the cursor's current leaf key (pCur->info.nKey).  Returns the byte
** length on a clean hit, or -1 on miss/error.  The caller must already have
** set up the leaf-less keying (BTCF_BfLeaf + bfLeaf) so bfCursorLeafPgno
** resolves to the un-read leaf.
*/
int sqlite3BfBtreeCachedPayloadSize(BtCursor *pCur){
  BtShared *pBt;
  BfCache  *pBf;
  u32       leaf;
  u8        keyBuf[8];
  int       nBuf = 0, rc;

  if( !pCur || !pCur->pBt || !pCur->curIntKey ) return -1;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return -1;
  leaf = bfCursorLeafPgno(pCur);
  if( leaf==0 ) return -1;
  bfEncodeRowid(pCur->info.nKey, keyBuf);
  rc = sqlite3BfRecordRead(pBf, leaf, keyBuf, 8, 0, &nBuf);
  return rc==BF_OK ? nBuf : -1;
}

/*
** Descent shortcut fast read: copy the clean cached record for the BF-served
** cursor's leaf key into pBuf (capacity nCap) in a SINGLE mini-page read, and
** return the record length (>=0).  Returns -1 on miss/tombstone/oversize.  Used
** by the serve path to avoid a separate size probe + fetch (two reads).
*/
int sqlite3BfBtreeReadCachedRecord(BtCursor *pCur, void *pBuf, int nCap){
  BtShared *pBt;
  BfCache  *pBf;
  u32       leaf;
  u8        keyBuf[8];
  int       nBuf = nCap, rc;

  if( !pCur || !pCur->pBt || !pCur->curIntKey || !pBuf || nCap<=0 ) return -1;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return -1;
  leaf = bfCursorLeafPgno(pCur);
  if( leaf==0 ) return -1;
  bfEncodeRowid(pCur->info.nKey, keyBuf);
  rc = sqlite3BfRecordRead(pBf, leaf, keyBuf, 8, pBuf, &nBuf);
  if( rc!=BF_OK ) return -1;        /* miss or BF_DELETED */
  if( nBuf>nCap ) return -1;        /* record larger than buffer: refuse */
  return nBuf;
}

/*
** Materialise a BF-served (BTCF_BfLeaf) cursor back onto its physical leaf
** page, by clearing the flag and re-running the descent with the shortcut
** suppressed.  Used by stepping paths (Next/Prev) that must operate on a real
** leaf.  No-op for a cursor that is not BF-served.
**
** A clean BFOP_CACHE record always shadows a row that exists in the base tree,
** so the re-descent lands on the real cell (*pRes==0); if the row has since
** vanished the normal not-found positioning applies, which Next/Prev handle.
*/
/*
** Adjust the descent-shortcut suppression counter for the cursor's cache.
** `delta` is +1 to suppress, -1 to release.  Used by the range-seek wrapper to
** keep whole-scan seeks on the full-descent path (the shortcut is a point-read
** optimisation; a scan would just materialise on the first Next anyway).
*/
void sqlite3BfBtreeSuppressShortcut(BtCursor *pCur, int delta){
  BtShared *pBt;
  BfCache  *pBf;
  if( !pCur || !pCur->pBt ) return;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( pBf ) pBf->bShortcutSuppressed += delta;
}

int sqlite3BfBtreeMaterializeLeaf(BtCursor *pCur){
  BtShared *pBt;
  BfCache  *pBf;
  i64       rowid;
  int       rc, res = 0;

  if( !pCur || (pCur->curFlags & BTCF_BfLeaf)==0 ) return SQLITE_OK;
  rowid = pCur->info.nKey;
  /* Clear BTCF_BfLeaf AND the cached-key claim.  A BF-served cursor is parked
  ** on the PARENT interior page (the leaf was never read), yet it carries
  ** BTCF_ValidNKey + info.nKey==rowid.  Left set, sqlite3BtreeTableMoveto's
  ** "already positioned" fast path (see btree.c) returns immediately without
  ** re-descending, leaving the cursor on the interior page; a subsequent
  ** Next then moveToLeftmost's from there and walks the WHOLE leftmost leaf,
  ** yielding rows below the seek bound and duplicates.  Dropping the claim
  ** forces a genuine descent onto the physical leaf cell. */
  pCur->curFlags &= ~(BTCF_BfLeaf|BTCF_ValidNKey);
  pBt = pCur->pBt;
  pBf = pBt ? btreeGetBfCache(pBt) : 0;
  if( pBf ) pBf->bShortcutSuppressed++;
  rc = sqlite3BtreeTableMoveto(pCur, rowid, 0, &res);
  if( pBf ) pBf->bShortcutSuppressed--;
  return rc;
}

/*
** Index-table BF record caching was removed (Stage 0.1 of the ablation
** plan).  It was already disabled at the call sites in an earlier bugfix
** round (the IndexMoveto override and the dead index tombstone/promotion
** writes were taken out), so the serialising functions it relied on
** (formerly sqlite3BfBtreeRecordExistsIndex /
** sqlite3BfBtreeCachePhantomIndex) were unreachable.
**
** Those functions serialised an UnpackedRecord into a byte string with no
** type tags (an INTEGER and a REAL could collide), silently skipped fields
** that overflowed the fixed key buffer (two long keys collided), and used
** ambiguous NULL/empty-string encodings.  Such a non-injective key would
** make BF answer existence/phantom checks wrongly.  Re-adding index caching
** therefore requires an *injective* encoder: a type-tag byte per field,
** hard failure (refuse to cache) instead of field skipping on overflow, and
** collation-aware comparison.  Do not re-introduce the old encoder.
*/

/*
** Promote a record read from the base page into the BF cache (BFOP_CACHE).
** Probabilistic — governed by promotionRate.
*/
int sqlite3BfBtreePromoteRecord(
  BtCursor *pCur,
  const void *pKey,
  int nKey,
  const void *pData,
  int nData
){
  BtShared *pBt;
  BfCache  *pBf;

  if( !pCur || !pCur->pBt ) return SQLITE_OK;
#if defined(SQLITE_BF_INSERT_BUFFERING)
  /* Never promote while a merge scan is in flight: promotion would write a
  ** BFOP_CACHE record into the same mini-page the scan is iterating, shifting
  ** the sorted meta array under pCur->bfIx.  Suppressing it keeps the dirty
  ** mini-page stable for the duration of the scan. */
  if( pCur->bfMerge ) return SQLITE_OK;
#endif
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return SQLITE_OK;

  /* Read the rate from the one place PRAGMA bf_promotion_rate writes it.
  **
  ** This used to test BfCache.promotionRate, a per-cache field that was
  ** declared and read here and assigned NOWHERE -- sqlite3MallocZero left it 0
  ** for the life of every cache, so this early-return fired on every read and
  ** read-path promotion never executed once.  The record cache was therefore
  ** populated only by writes: a read-only workload showed a 0.0% record hit
  ** rate (and the promotion sweep in results/sweeps returned six identical
  ** rows, because the knob was inert).  The field is gone; the global config
  ** value is the only source of truth, which also makes the pragma live. */
  {
    int rate = sqlite3BfCachePromotionRate();
    if( rate<=0 ) return SQLITE_OK;
    if( rate<100 ){
      u32 r=0;
      sqlite3_randomness(4, &r);
      if( (r%100)>=(u32)rate ) return SQLITE_OK;
    }
  }

  /* Promote into the mini-page of the leaf the row lives on. */
  {
    u32 leaf = bfCursorLeafPgno(pCur);
    if( leaf==0 ) return SQLITE_OK;

    /* Re-encode rowid key if passed raw. */
    if( nKey==sizeof(i64) && pCur->curIntKey ){
      u8 keyBuf[8];
      i64 rowid; memcpy(&rowid,pKey,sizeof(i64));
      bfEncodeRowid(rowid, keyBuf);
      sqlite3BfRecordWrite(pBf, leaf, keyBuf, 8, pData, nData, BFOP_CACHE);
      bfTagLeafRoot(pBf, leaf, pCur->pgnoRoot);
      return SQLITE_OK;
    }

    sqlite3BfRecordWrite(pBf, leaf, pKey, nKey, pData, nData, BFOP_CACHE);
    bfTagLeafRoot(pBf, leaf, pCur->pgnoRoot);
  }
  return SQLITE_OK;
}

/*
** Cache a negative lookup (phantom) for a rowid table.
*/
int sqlite3BfBtreeCachePhantom(
  BtCursor *pCur,
  const void *pKey,
  int nKey
){
  BtShared *pBt;
  BfCache  *pBf;

  if( !pCur || !pCur->pBt ) return SQLITE_OK;
  pBt = pCur->pBt;
  pBf = btreeGetBfCache(pBt);
  if( !pBf ) return SQLITE_OK;

  /* Cache the phantom on the leaf where the key would live. */
  {
    u32 leaf = bfCursorLeafPgno(pCur);
    if( leaf==0 ) return SQLITE_OK;

    /* Re-encode rowid if raw. */
    if( nKey==sizeof(i64) && pCur->curIntKey ){
      u8 keyBuf[8];
      i64 rowid; memcpy(&rowid,pKey,sizeof(i64));
      bfEncodeRowid(rowid, keyBuf);
      sqlite3BfRecordWrite(pBf, leaf, keyBuf, 8, "", 0, BFOP_PHANTOM);
      bfTagLeafRoot(pBf, leaf, pCur->pgnoRoot);
      return SQLITE_OK;
    }

    sqlite3BfRecordWrite(pBf, leaf, pKey, nKey, "", 0, BFOP_PHANTOM);
    bfTagLeafRoot(pBf, leaf, pCur->pgnoRoot);
  }
  return SQLITE_OK;
}

/*
** Write-through cache for a rowid record.  Called by the insert path BEFORE
** the base-page write, while the cursor is reliably positioned on the leaf the
** row belongs to (no balance has run yet).  Writing the new value here:
**   - overwrites any stale BFOP_CACHE/BFOP_PHANTOM for this rowid on the leaf
**     (so an insert after a probe-miss phantom, or an overwrite/update, can
**     never leave a stale read-cache entry — the coherence bug that a
**     post-balance write-through could not fix, since balance may move the
**     cursor off the leaf);
**   - is split-safe: if a later balance moves the row to a sibling leaf this
**     entry becomes unreachable (reads descend to the row's actual leaf and
**     re-promote from the base) rather than wrong.
** No-op when the cursor is not on a usable leaf — a later read just promotes.
*/
int sqlite3BfBtreeCacheRecord(
  BtCursor *pCur,
  i64 rowid,
  const void *pData,
  int nData
){
  BfCache *pBf;
  u32 leaf;
  u8  keyBuf[8];

  if( !pCur || !pCur->pBt || !pCur->curIntKey ) return SQLITE_OK;
  if( pData==0 || nData<=0 || nData>(int)BF_MAX_MINI_PAGE ) return SQLITE_OK;
  pBf = btreeGetBfCache(pCur->pBt);
  if( !pBf ) return SQLITE_OK;
  leaf = bfCursorLeafPgno(pCur);
  if( leaf==0 ) return SQLITE_OK;

  bfEncodeRowid(rowid, keyBuf);
  (void)sqlite3BfRecordWrite(pBf, leaf, keyBuf, 8, pData, nData, BFOP_CACHE);
  bfTagLeafRoot(pBf, leaf, pCur->pgnoRoot);
  return SQLITE_OK;
}

/*
** Drop any BF mini-page mapped under pgno.  Called from freePage2 when a page
** leaves a table: its pgno may be recycled as a different leaf, so a stale
** read-cache keyed by it must not survive to serve the new page's contents.
** Goes straight to the pager's cache (not btreeGetBfCache) so it still forgets
** while a bypass flush is active; Phase 0/1 entries are all clean, so this is
** an unconditional, always-safe drop.
*/
void sqlite3BfBtreeForgetPage(BtShared *pBt, Pgno pgno){
  BfCache    *pBf;
  BfMapEntry *pEntry;

  if( !pBt || pgno<=1 ) return;
  pBf = sqlite3PagerGetBfCache(pBt->pPager);
  if( !pBf ) return;
  pEntry = sqlite3BfMapLookup(pBf, pgno);
  if( pEntry && pEntry->locType==BF_LOC_MINI ){
    /* Drop only CLEAN mini-pages.  A dirty mini-page (buffered BFOP_INSERT /
    ** BFOP_DELETE) must never be silently dropped — that would lose data the
    ** base tree does not hold (invariant §1.3.2).  Callers (balance_nonroot,
    ** freePage2) run either:
    **   - outside a flush (bBypassActive==0): a pre-flush at the start of the
    **     mutating insert/delete has already materialised this table's dirty
    **     records, so nothing dirty remains here; OR
    **   - inside a flush replay (bBypassActive==1): the still-dirty siblings
    **     are queued in the flush-all sweep and are read out of their slabs by
    **     rowid, so leaving them keyed to a now-stale leaf is harmless (reads
    **     are bypassed until the sweep finishes).
    ** Either way, leaving a dirty mini-page in place is correct; dropping it
    ** is not. */
    if( !sqlite3BfMiniPageIsDirty((BfMiniPage*)pEntry->pPage) ){
      pEntry->locType = BF_LOC_NULL;
      pEntry->pPage   = 0;
    }else{
      /* Phase 2 makes the comment above only half true: records stay DIRTY
      ** across commits by design, so a mini-page here often IS dirty and used
      ** to be left completely untouched -- stale CLEAN records included.  Those
      ** are the dangerous ones: a clean record on a leaf that no longer owns
      ** the key answers point probes with a row that may since have been
      ** deleted (measured: a re-INSERT failing with "UNIQUE constraint failed"
      ** after a rebalance handed the range back).  Drop the clean records and
      ** keep the dirty ones, which are position-independent. */
#ifndef SQLITE_BF_NO_DROPCLEAN
      /* NOT during a flush replay: bfFlushOneMiniPage is iterating this very
      ** mini-page by index, and applying its records to the base tree is what
      ** triggered this balance.  Rewriting the page underneath that iteration
      ** shifts the meta array and makes the flush skip dirty records -- i.e.
      ** silently lose committed rows (measured before this guard went in).
      ** The flush marks the page clean when it finishes, so the stale clean
      ** records this leaves behind are dropped by the next ForgetPage. */
      if( !pBf->bBypassActive ){
        sqlite3BfMiniPageDropClean((BfMiniPage*)pEntry->pPage);
      }else{
        /* Defer: the flush finishes the job (see bfFlushOneMiniPage). */
        ((BfMiniPage*)pEntry->pPage)->flags |= BF_MINI_F_STALE;
      }
#endif
    }
  }
}

#ifdef SQLITE_DEBUG
/*
** Debug safety net for the Stage 1.5 page-lifecycle invariant: relocatePage
** (auto-vacuum) must never move a page that carries a BF mini-page, because the
** cache is keyed by pgno and relocation would orphan/corrupt it.  BF is disabled
** outright for auto-vacuum databases (btreeUsesBfCache), so this must hold; the
** assert in relocatePage catches any future regression that re-enables BF there.
** Returns 1 if a mini-page is mapped under pgno, else 0.
*/
int sqlite3BfBtreeDebugHasMiniPage(BtShared *pBt, Pgno pgno){
  BfCache    *pBf;
  BfMapEntry *pEntry;
  if( !pBt ) return 0;
  pBf = sqlite3PagerGetBfCache(pBt->pPager);
  if( !pBf ) return 0;
  pEntry = sqlite3BfMapLookup(pBf, pgno);
  return (pEntry && pEntry->locType==BF_LOC_MINI && pEntry->pPage) ? 1 : 0;
}
#endif /* SQLITE_DEBUG */

/* Index-table phantom caching (formerly sqlite3BfBtreeCachePhantomIndex)
** was removed in Stage 0.1; see the note above sqlite3BfBtreePromoteRecord
** for why and what a correct re-implementation would require. */

/*
** Cache statistics for PRAGMA bf_cache_stats.
*/
void sqlite3BfBtreeStats(
  Btree *p,
  u64 *pMiniPageHit,
  u64 *pMiniPageMiss,
  u64 *pUpgrades,
  u64 *pMerges,
  u64 *pEvictions
){
  BfCache *pBf = 0;
  if( p && p->pBt ) pBf = btreeGetBfCache(p->pBt);
  if( pMiniPageHit )  *pMiniPageHit  = pBf ? pBf->nMiniPageHit  : 0;
  if( pMiniPageMiss ) *pMiniPageMiss = pBf ? pBf->nMiniPageMiss : 0;
  if( pUpgrades )     *pUpgrades     = pBf ? pBf->nUpgrades     : 0;
  if( pMerges )       *pMerges       = pBf ? pBf->nMergeToBase  : 0;
  if( pEvictions )    *pEvictions    = pBf ? pBf->cb.nEvictions : 0;
}

/* CLOCK second chances granted (PRAGMA bf_cache_stats).  Read it next to
** evictions: the ratio says how much of the sweep is retention work. */
void sqlite3BfBtreeClockStat(Btree *p, u64 *pnSpared){
  BfCache *pBf = 0;
  if( p && p->pBt ) pBf = btreeGetBfCache(p->pBt);
  if( pnSpared ) *pnSpared = pBf ? pBf->nClockSpared : 0;
}

/*
** pcache2 page-hash statistics for PRAGMA bf_cache_stats.
**
** Reported separately from mini_page_hits: the pager's BfCache IS the pcache2
** instance (see sqlite3PagerOpenBfCache), so folding xFetch hits into the
** record counters made the record-cache hit rate unreadable.
*/
void sqlite3BfBtreePageCacheStats(Btree *p, u64 *pHit, u64 *pMiss){
  BfCache *pBf = 0;
  if( p && p->pBt ) pBf = btreeGetBfCache(p->pBt);
  if( pHit )  *pHit  = pBf ? pBf->nPageFetchHit  : 0;
  if( pMiss ) *pMiss = pBf ? pBf->nPageFetchMiss : 0;
}

/*
** Space gauges for the record cache -- what it COSTS, next to what it saves.
** See sqlite3BfMapSpaceStats.  Reported by PRAGMA bf_cache_stats; these are
** live gauges, not cumulative counters, so a caller sampling them before and
** after a run must not difference them.
*/
void sqlite3BfBtreeSpaceStats(
  Btree *p,
  u64 *pnBatches,
  u64 *pnEntries,
  u64 *pnMiniPages,
  u64 *pnRecords,
  u64 *pnMiniBytes,
  u64 *pnCapacity
){
  BfCache *pBf = 0;
  if( p && p->pBt ) pBf = btreeGetBfCache(p->pBt);
  sqlite3BfMapSpaceStats(pBf, pnBatches, pnEntries, pnMiniPages,
                         pnRecords, pnMiniBytes, pnCapacity);
}

/*
** Merge-scan statistics (Stage 2.2) for PRAGMA bf_cache_stats: number of
** forward scans that armed merge-iteration and number of buffered inserts the
** merge located.
*/
void sqlite3BfBtreeMergeStats(Btree *p, u64 *pScans, u64 *pInserts, u64 *pBail){
  BfCache *pBf = 0;
  if( p && p->pBt ) pBf = btreeGetBfCache(p->pBt);
  if( pScans )   *pScans   = pBf ? pBf->nMergeScans   : 0;
  if( pInserts ) *pInserts = pBf ? pBf->nMergeInserts : 0;
  if( pBail )    *pBail    = pBf ? pBf->nMergeBail    : 0;
}

/*
** Note that an insert on a rowid table took the base-page write path instead of
** being absorbed by a mini-page (mini-page full, cursor not on a usable leaf,
** an overwrite, or a flag combination the buffering gate excludes).  Every such
** insert costs a page-image WAL frame at commit, so the ratio
** buffered_inserts : insert_fallbacks is what the write win is made of.
*/
void sqlite3BfBtreeNoteInsertFallback(BtCursor *pCur){
  BfCache *pBf;
  if( !pCur || !pCur->pBt ) return;
  pBf = btreeGetBfCache(pCur->pBt);
  if( pBf ) pBf->nInsertFallback++;
}

/*
** Insert-path statistics (Phase 2) for PRAGMA bf_cache_stats.
*/
void sqlite3BfBtreeInsertStats(Btree *p, u64 *pBuffered, u64 *pFallback,
                               u64 *pRefused, u64 *pCompactions){
  BfCache *pBf = 0;
  if( p && p->pBt ) pBf = btreeGetBfCache(p->pBt);
  if( pBuffered )    *pBuffered    = pBf ? pBf->nBufferedInserts : 0;
  if( pFallback )    *pFallback    = pBf ? pBf->nInsertFallback  : 0;
  if( pRefused )     *pRefused     = pBf ? pBf->nInsertRefused   : 0;
  if( pCompactions ) *pCompactions = pBf ? pBf->nCompactions     : 0;
}

/*
** WAL write-amplification statistics (Phase 2) for PRAGMA bf_cache_stats.
** wal_record_frames = record-batch frames this connection's WAL has written;
** wal_page_frames   = 4 KB page images it had to write anyway;
** wal_commits       = commit frames (i.e. transactions) written.
** The Phase-2 write claim is page_frames/commits staying at ~1 (the commit
** frame itself) while the row data rides record frames.
*/
void sqlite3BfBtreeWalStats(Btree *p, u64 *pRecFrames, u64 *pPageFrames,
                            u64 *pCommits){
  if( pRecFrames )  *pRecFrames  = 0;
  if( pPageFrames ) *pPageFrames = 0;
  if( pCommits )    *pCommits    = 0;
  if( p && p->pBt && p->pBt->pPager ){
    sqlite3PagerBfFrameStats(p->pBt->pPager, pRecFrames, pPageFrames, pCommits);
  }
}

/*
** Write-back delete statistics (Stage 2.3) for PRAGMA bf_cache_stats: number of
** deletes buffered as BFOP_DELETE tombstones, and number of base cells the
** forward merge scan suppressed because of a tombstone.
*/
void sqlite3BfBtreeDeleteStats(Btree *p, u64 *pWriteBack, u64 *pTombstones){
  BfCache *pBf = 0;
  if( p && p->pBt ) pBf = btreeGetBfCache(p->pBt);
  if( pWriteBack )  *pWriteBack  = pBf ? pBf->nWriteBackDeletes : 0;
  if( pTombstones ) *pTombstones = pBf ? pBf->nMergeTombstones  : 0;
}

/* -------------------------------------------------------------------------
** Legacy apply stubs (called from bf_cache.c merge callback; noop when
** pgnoRootForFlush is not set — the real work is done by bfFlushOneMiniPage)
** ----------------------------------------------------------------------- */

int sqlite3BfBtreeApplyInsert(
  Pager *pPager, Pgno pgno,
  const void *pKey, int nKey,
  const void *pVal,  int nVal
){
  UNUSED_PARAMETER(pPager); UNUSED_PARAMETER(pgno);
  UNUSED_PARAMETER(pKey); UNUSED_PARAMETER(nKey);
  UNUSED_PARAMETER(pVal); UNUSED_PARAMETER(nVal);
  /* Real apply is handled by bfFlushOneMiniPage via the bypass cursor.
  ** sqlite3BfCacheMergeWithPager calls this only when pgnoRootForFlush is
  ** set, but that path is superseded by the direct flush above. */
  return SQLITE_OK;
}

int sqlite3BfBtreeApplyDelete(
  Pager *pPager, Pgno pgno,
  const void *pKey, int nKey
){
  UNUSED_PARAMETER(pPager); UNUSED_PARAMETER(pgno);
  UNUSED_PARAMETER(pKey); UNUSED_PARAMETER(nKey);
  return SQLITE_OK;
}

#endif /* SQLITE_OMIT_BF_CACHE */
