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
** This file implements configuration and PRAGMA support for Bf-Tree cache.
**
** PRAGMAs are the ONLY configuration surface.  A sqlite3_config() path
** (SQLITE_CONFIG_BFCACHE / _SIZE) was declared here and never wired to
** anything; it was removed 2026-09-21 along with its opcodes, which collided
** with SQLite's own.
**
** Pragmas:
**   PRAGMA bf_cache              - Enable/disable for connection
**   PRAGMA bf_cache_size         - Get/set cache size
**   PRAGMA bf_cache_stats        - Show cache statistics
**   PRAGMA bf_promotion_rate     - Get/set read promotion rate
**   PRAGMA bf_deferred_commit    - Bounded deferred durability window (N txns)
**   PRAGMA bf_group_commit       - Old name of bf_deferred_commit, kept
**   PRAGMA bf_min_record         - Size-class ladder base
**   PRAGMA bf_copy_on_access     - Copy-on-access region, percent
*/
#include "sqliteInt.h"
#include "bf_cache.h"

#ifndef SQLITE_OMIT_BF_CACHE

/*
** Configuration values for Bf-Tree cache.
*/
typedef struct BfConfig {
  int bEnabled;               /* Bf-Tree cache enabled globally */
  u64 nBufferSize;            /* Circular buffer size in bytes */
  int nPromotionRate;         /* Read promotion rate (0-100) */
  double copyOnAccessRatio;   /* Copy-on-access threshold (0.0-1.0) */
  int nGroupCommit;           /* Transactions per record-frame group (0/1=off) */
  int nMinRecord;             /* Size-class ladder base; see bf_min_record */
} BfConfig;

static BfConfig bfConfig = {
  1,                          /* Enabled by default */
  BF_DEFAULT_BUFFER_SIZE,     /* 32 MB default */
  BF_DEFAULT_PROMOTION_RATE,  /* see bf_cache.h for why 30 */
  BF_DEFAULT_COPY_ON_ACCESS,  /* 10% copy-on-access region */
  0,                          /* Group commit off: one flush per commit */
  BF_DEFAULT_MIN_RECORD       /* ours, measured; the reference's default is 4 */
};

/*
** Number of transactions a record-frame group may span (Phase 2 group commit).
** 0 or 1 means every commit writes its own record frame(s) immediately.
*/
int sqlite3BfGroupCommitTxns(void){
  return bfConfig.nGroupCommit;
}

/*
** Check if Bf-Tree cache is globally enabled.
*/
int sqlite3BfCacheEnabled(void){
  return bfConfig.bEnabled;
}

/*
** Get the configured buffer size.
*/
u64 sqlite3BfCacheBufferSize(void){
  return bfConfig.nBufferSize;
}

/*
** Base record size for the mini-page size-class ladder.
** The reference's cb_min_record_size (config.rs), which its benchmark sets per
** workload -- which is the whole reason this is configuration and not a
** constant.  See PRAGMA bf_min_record below.
*/
int sqlite3BfCacheMinRecord(void){
  return bfConfig.nMinRecord;
}

/*
** Size of the copy-on-access region as a fraction of the ring (0.0-1.0).
** The reference's cb_copy_on_access_ratio; see PRAGMA bf_copy_on_access.
*/
double sqlite3BfCacheCopyOnAccessRatio(void){
  return bfConfig.copyOnAccessRatio;
}

/*
** Get the promotion rate.
*/
int sqlite3BfCachePromotionRate(void){
  return bfConfig.nPromotionRate;
}

/*
** Implementation of PRAGMA bf_cache
**
** PRAGMA bf_cache;           -- Returns current enable state
** PRAGMA bf_cache = ON;      -- Enable for this connection
** PRAGMA bf_cache = OFF;     -- Disable for this connection
*/
void sqlite3PragmaBfCache(
  Parse *pParse,
  const char *zDb,
  const char *zValue
){
  Vdbe *v = sqlite3GetVdbe(pParse);
  sqlite3 *db = pParse->db;

  if( zValue == 0 ){
    /* Query current state */
    pParse->nMem = MAX(pParse->nMem, 1);
    sqlite3VdbeSetNumCols(v, 1);
    sqlite3VdbeAddOp2(v, OP_Integer, bfConfig.bEnabled, 1);
    sqlite3VdbeSetColName(v, 0, COLNAME_NAME, "bf_cache", SQLITE_STATIC);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 1);
  }else{
    /* Set state */
    int bEnable = sqlite3GetBoolean(zValue, 0);
    if( bEnable ){
      bfConfig.bEnabled = 1;
      sqlite3BfCacheSetMethods();
    }else{
      bfConfig.bEnabled = 0;
      /* No global cache to tear down: caches are per pager.  The call that
      ** used to be here operated on bfGlobalCache, which nothing ever
      ** assigned, so it was a mutex acquire and nothing else. */
      sqlite3PCacheSetDefault();
    }
  }
}

/*
** Implementation of PRAGMA bf_cache_size
**
** PRAGMA bf_cache_size;          -- Returns current buffer size
** PRAGMA bf_cache_size = N;      -- Set buffer size (in bytes)
*/
void sqlite3PragmaBfCacheSize(
  Parse *pParse,
  const char *zDb,
  const char *zValue
){
  Vdbe *v = sqlite3GetVdbe(pParse);

  if( zValue == 0 ){
    /* Query current size */
    pParse->nMem = MAX(pParse->nMem, 1);
    sqlite3VdbeSetNumCols(v, 1);
    sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 1, 0,
                          (const u8*)&bfConfig.nBufferSize, P4_INT64);
    sqlite3VdbeSetColName(v, 0, COLNAME_NAME, "bf_cache_size", SQLITE_STATIC);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 1);
  }else{
    /* Set size */
    i64 nSize = 0;
    sqlite3Atoi64(zValue, &nSize, sqlite3Strlen30(zValue), SQLITE_UTF8);
    if( nSize >= BF_MAX_MINI_PAGE * 2 ){
      u64 n = 1;
      while( n < (u64)nSize ) n *= 2;
      bfConfig.nBufferSize = n;

      /* Apply it to the cache this connection already has.  Setting the global
      ** alone is not enough: the cache is created during the schema load, long
      ** before any BF pragma can run, so without this the new size would only
      ** ever reach caches created later -- which, for the connection issuing
      ** the pragma, means never.  See sqlite3BfBtreeResizeCache. */
      {
        sqlite3 *db = pParse->db;
        int iDb = sqlite3FindDbName(db, zDb);
        Btree *pBt = db->aDb[iDb>=0 ? iDb : 0].pBt;
        extern int sqlite3BfBtreeResizeCache(Btree*, u64);
        if( pBt ) (void)sqlite3BfBtreeResizeCache(pBt, n);
      }
    }
  }
}

/*
** Implementation of PRAGMA bf_cache_stats
**
** PRAGMA bf_cache_stats;     -- Returns cache statistics
*/
void sqlite3PragmaBfCacheStats(
  Parse *pParse,
  const char *zDb
){
  Vdbe *v = sqlite3GetVdbe(pParse);
  int i;

  pParse->nMem = MAX(pParse->nMem, 2);
  sqlite3VdbeSetNumCols(v, 2);

  /* Column names */
  static const char *azCols[] = {
    "stat", "value"
  };

  for(i = 0; i < 2; i++){
    sqlite3VdbeSetColName(v, i, COLNAME_NAME, azCols[i], SQLITE_STATIC);
  }

  /* Add statistics rows */
  sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "enabled", P4_STATIC);
  sqlite3VdbeAddOp2(v, OP_Integer, bfConfig.bEnabled, 2);
  sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

  sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "buffer_size", P4_STATIC);
  sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                        (const u8*)&bfConfig.nBufferSize, P4_INT64);
  sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

  sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "promotion_rate", P4_STATIC);
  sqlite3VdbeAddOp2(v, OP_Integer, bfConfig.nPromotionRate, 2);
  sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

  {
    sqlite3 *db = pParse->db;
    int iDb = sqlite3FindDbName(db, zDb);
    Btree *pBt = db->aDb[iDb>=0 ? iDb : 0].pBt;
    u64 nHit = 0, nMiss = 0, nUpgrade = 0, nMerge = 0, nEvict = 0;
    if( pBt ){
      extern void sqlite3BfBtreeStats(Btree*, u64*, u64*, u64*, u64*, u64*);
      sqlite3BfBtreeStats(pBt, &nHit, &nMiss, &nUpgrade, &nMerge, &nEvict);
    }
    sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "mini_page_hits", P4_STATIC);
    sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nHit, P4_INT64);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

    sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "mini_page_misses", P4_STATIC);
    sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nMiss, P4_INT64);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

    {
      /* The pcache2 layer, kept apart from the record counters above. */
      u64 nPgHit = 0, nPgMiss = 0;
      if( pBt ){
        extern void sqlite3BfBtreePageCacheStats(Btree*, u64*, u64*);
        sqlite3BfBtreePageCacheStats(pBt, &nPgHit, &nPgMiss);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "page_cache_hits", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nPgHit, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "page_cache_misses", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nPgMiss, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }

    {
      /* Space gauges: what the record cache costs, alongside what it saves.
      ** map_batches x 256 x sizeof(BfMapEntry) is the mapping table's
      ** footprint; comparing it to map_entries shows how much of that is
      ** actually occupied.  cached_records / mini_page_bytes gives the real
      ** bytes-per-cached-row, the number that decides whether a byte is better
      ** spent here or left to the OS page cache. */
      u64 nBatch=0, nEntry=0, nMini=0, nRec=0, nMiniB=0, nCap=0;
      if( pBt ){
        extern void sqlite3BfBtreeSpaceStats(Btree*, u64*, u64*, u64*,
                                             u64*, u64*, u64*);
        sqlite3BfBtreeSpaceStats(pBt, &nBatch, &nEntry, &nMini, &nRec, &nMiniB,
                                 &nCap);
      }
      /* The capacity the ring is ACTUALLY running with, as opposed to
      ** buffer_size above, which echoes the configured value.  They differ
      ** whenever the cache was instantiated before the configuration was
      ** applied, and only this one governs how much the cache can hold. */
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "cb_capacity", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nCap, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "map_batches", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nBatch, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "map_entries", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nEntry, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "live_mini_pages", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nMini, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "cached_records", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nRec, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "mini_page_bytes", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nMiniB, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }

    {
      u64 nCoaMoved = 0, nCoaShed = 0;
      if( pBt ){
        extern void sqlite3BfBtreeCopyOnAccessStat(Btree*, u64*, u64*);
        sqlite3BfBtreeCopyOnAccessStat(pBt, &nCoaMoved, &nCoaShed);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "copy_on_access", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nCoaMoved, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "copy_on_access_shed", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nCoaShed, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }

    {
      u64 nUpShed = 0;
      if( pBt ){
        extern void sqlite3BfBtreeUpgradeShedStat(Btree*, u64*);
        sqlite3BfBtreeUpgradeShedStat(pBt, &nUpShed);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "upgrade_shed", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nUpShed, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }

    {
      static const char *const azReplay[] = {
        "replay_applied", "replay_superseded", "replay_torn", "replay_dropped",
        "rollback_rehydrate", "clear_logged"
      };
      u64 aReplay[6];
      int iR;
      memset(aReplay, 0, sizeof(aReplay));
      if( pBt ){
        extern void sqlite3BfBtreeReplayStat(Btree*, u64*);
        sqlite3BfBtreeReplayStat(pBt, aReplay);
      }
      for(iR=0; iR<6; iR++){
        sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, azReplay[iR], P4_STATIC);
        sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&aReplay[iR],
                              P4_INT64);
        sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
      }
    }

    {
      u64 nDeFlush = 0, nDeRetry = 0, nDeRefused = 0, nDeSeen = 0;
      if( pBt ){
        extern void sqlite3BfBtreeDirtyEvictStat(Btree*, u64*, u64*, u64*, u64*);
        sqlite3BfBtreeDirtyEvictStat(pBt, &nDeFlush, &nDeRetry, &nDeRefused,
                                     &nDeSeen);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "evict_stall_seen", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nDeSeen, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "dirty_evict_flush", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nDeFlush, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "dirty_evict_retry", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nDeRetry, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "dirty_evict_refused", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nDeRefused,P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }

    sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "upgrades", P4_STATIC);
    sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nUpgrade, P4_INT64);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

    sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "merges", P4_STATIC);
    sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nMerge, P4_INT64);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

    sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "evictions", P4_STATIC);
    sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0, (const u8*)&nEvict, P4_INT64);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

    {
      u64 nMergeScans = 0, nMergeIns = 0, nMergeBail = 0;
      if( pBt ){
        extern void sqlite3BfBtreeMergeStats(Btree*, u64*, u64*, u64*);
        sqlite3BfBtreeMergeStats(pBt, &nMergeScans, &nMergeIns, &nMergeBail);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "merge_scans", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nMergeScans, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "merge_inserts", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nMergeIns, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "merge_bail", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nMergeBail, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }

    {
      u64 nWbDel = 0, nMergeTomb = 0;
      if( pBt ){
        extern void sqlite3BfBtreeDeleteStats(Btree*, u64*, u64*);
        sqlite3BfBtreeDeleteStats(pBt, &nWbDel, &nMergeTomb);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "writeback_deletes",P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nWbDel, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "merge_tombstones",P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nMergeTomb, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }

    {
      u64 nBufIns = 0, nFallback = 0, nRefused = 0, nCompact = 0;
      if( pBt ){
        extern void sqlite3BfBtreeInsertStats(Btree*, u64*, u64*, u64*, u64*);
        sqlite3BfBtreeInsertStats(pBt, &nBufIns, &nFallback, &nRefused,
                                  &nCompact);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "buffered_inserts", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nBufIns, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "insert_fallbacks", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nFallback, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      /* Subset of insert_fallbacks where BF itself refused the record; the
      ** remainder never reached BF (excluded by the buffering gate). */
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "insert_refused", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nRefused, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "minipage_compactions",
                        P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nCompact, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }

    {
      /* WAL write amplification (Phase 2): the write win is wal_page_frames
      ** staying at the per-commit floor while wal_record_frames carries the
      ** row data. */
      u64 nRecFr = 0, nPgFr = 0, nCommits = 0;
      if( pBt ){
        extern void sqlite3BfBtreeWalStats(Btree*, u64*, u64*, u64*);
        sqlite3BfBtreeWalStats(pBt, &nRecFr, &nPgFr, &nCommits);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "wal_record_frames",P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nRecFr, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "wal_page_frames", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nPgFr, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "wal_commits", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nCommits, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }

#if defined(SQLITE_BF_INSERT_BUFFERING)
    {
      /* Group commit: batches handed to the WAL, and commits that wrote
      ** nothing because their records rode a later group's frame. */
      u64 nStaged = 0, nDeferred = 0;
      int nPending = 0;
      if( pBt ){
        extern void sqlite3BfBtreeGroupStats(Btree*, u64*, u64*, int*);
        sqlite3BfBtreeGroupStats(pBt, &nStaged, &nDeferred, &nPending);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "group_batches", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nStaged, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "group_deferred", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nDeferred, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);
    }
#endif
  }
}

/*
** Implementation of PRAGMA bf_deferred_commit (old name: bf_group_commit)
**
** PRAGMA bf_deferred_commit;       -- Returns the current window (0 = off)
** PRAGMA bf_deferred_commit = N;   -- Defer up to N-1 commits' record frames
**
** BOUNDED DEFERRED DURABILITY -- not group commit (BF_TREE_V2_PLAN.md D2, owner
** decision 2026-09-24).  With N>1 a RECORD-ONLY commit appends its records to
** an open batch and writes nothing; the batch is written by the N-th commit, or
** earlier if it fills, if a base-page flush forces it out, or if a commit also
** writes pages (which never defers: that would tear the transaction).  So an
** acknowledged commit may be lost -- up to N-1 of them -- on a process crash as
** well as a power loss.  The reference does not do this: its committer blocks
** until its record is written (no fsync), which is what N<=1 does here, with
** PRAGMA synchronous=FULL adding an fsync.  The WAL-volume saving of N>1 is
** entirely the deferral; report it as such.
*/
void sqlite3PragmaBfGroupCommit(
  Parse *pParse,
  const char *zDb,
  const char *zValue
){
  Vdbe *v = sqlite3GetVdbe(pParse);
  UNUSED_PARAMETER(zDb);

  if( zValue == 0 ){
    pParse->nMem = MAX(pParse->nMem, 1);
    sqlite3VdbeSetNumCols(v, 1);
    sqlite3VdbeAddOp2(v, OP_Integer, bfConfig.nGroupCommit, 1);
    sqlite3VdbeSetColName(v, 0, COLNAME_NAME, "bf_group_commit", SQLITE_STATIC);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 1);
  }else{
    int n = sqlite3Atoi(zValue);
    if( n < 0 ) n = 0;
    if( n > BF_MAX_GROUP_COMMIT ) n = BF_MAX_GROUP_COMMIT;
    bfConfig.nGroupCommit = n;
  }
}

/*
** Implementation of PRAGMA bf_promotion_rate
**
** PRAGMA bf_promotion_rate;      -- Returns current rate (0-100)
** PRAGMA bf_promotion_rate = N;  -- Set rate
*/
void sqlite3PragmaBfPromotionRate(
  Parse *pParse,
  const char *zDb,
  const char *zValue
){
  Vdbe *v = sqlite3GetVdbe(pParse);

  if( zValue == 0 ){
    /* Query current rate */
    pParse->nMem = MAX(pParse->nMem, 1);
    sqlite3VdbeSetNumCols(v, 1);
    sqlite3VdbeAddOp2(v, OP_Integer, bfConfig.nPromotionRate, 1);
    sqlite3VdbeSetColName(v, 0, COLNAME_NAME, "bf_promotion_rate", SQLITE_STATIC);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 1);
  }else{
    /* Set rate */
    int rate = sqlite3Atoi(zValue);
    if( rate < 0 ) rate = 0;
    if( rate > 100 ) rate = 100;
    bfConfig.nPromotionRate = rate;
  }
}

/*
** Implementation of PRAGMA bf_min_record
**
** PRAGMA bf_min_record;          -- Returns the ladder's base record size
** PRAGMA bf_min_record = N;      -- Set it (bytes)
**
** The size classes are DERIVED from this: class(k) = 2^k * (N + sizeof(KVMeta))
** + sizeof(BfMiniPage), cache-line aligned (tree.rs:222-250).  A ladder is only
** well matched to records near N, so this has to be configuration -- the
** reference has it as cb_min_record_size and its benchmark sets it per
** workload.
**
** MEASURED, 2026-09-16, 4M rows under a 16 MiB ring, N=64 against the old
** power-of-two ladder -- the same change helps or hurts depending on how a
** record lands relative to a class boundary:
**
**   value_len  recs/page   B/record        cached
**       32        5.4        -2.5%          +1.4%
**       64        2.3         -12%         +11.7%
**      100        1.5       -23.6%         +27.7%
**      200        1.2       +23.6%         -18.6%   <- REGRESSION
**
** So leaving N at a constant tuned for one workload silently penalises the
** others; a 200-byte value loses 19% of the records it could cache.  Set it to
** the workload's typical record size.
*/
void sqlite3PragmaBfMinRecord(
  Parse *pParse,
  const char *zDb,
  const char *zValue
){
  Vdbe *v = sqlite3GetVdbe(pParse);

  if( zValue == 0 ){
    pParse->nMem = MAX(pParse->nMem, 1);
    sqlite3VdbeSetNumCols(v, 1);
    sqlite3VdbeAddOp2(v, OP_Integer, bfConfig.nMinRecord, 1);
    sqlite3VdbeSetColName(v, 0, COLNAME_NAME, "bf_min_record", SQLITE_STATIC);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 1);
  }else{
    int n = sqlite3Atoi(zValue);
    /* Bounded so the derived ladder stays sane: the first class must hold a
    ** header plus one record, and the largest must still fit a full page. */
    if( n < 8 ) n = 8;
    if( n > (int)(BF_MAX_MINI_PAGE/4) ) n = (int)(BF_MAX_MINI_PAGE/4);
    if( n != bfConfig.nMinRecord ){
      bfConfig.nMinRecord = n;
      /* Same reason as bf_cache_size: the cache is created during the schema
      ** load, long before any BF pragma can run, so setting the global alone
      ** would never reach the cache the connection issuing this pragma is
      ** using.  Rebuilding the ladder under live allocations is not safe --
      ** blocks were filed under the OLD class sizes -- so the setter drops
      ** every mapping and reinitialises the ring, exactly as a resize does. */
      {
        sqlite3 *db = pParse->db;
        int iDb = sqlite3FindDbName(db, zDb);
        Btree *pBt = db->aDb[iDb>=0 ? iDb : 0].pBt;
        extern int sqlite3BfBtreeSetMinRecord(Btree*, u32);
        if( pBt ) (void)sqlite3BfBtreeSetMinRecord(pBt, (u32)n);
      }
    }
  }
}

/*
** Implementation of PRAGMA bf_copy_on_access
**
** PRAGMA bf_copy_on_access;        -- Returns the region size, in PERCENT
** PRAGMA bf_copy_on_access = N;    -- Set it (0-100)
**
** The copy-on-access second chance (paper 4.1; the reference's
** cb_copy_on_access_ratio, config.rs:47).  N is the head-most percentage of
** the ring in which a mini-page that is READ gets relocated to the tail,
** shedding its cold cache records on the way (sqlite3BfCacheCopyOnAccess).
**
**   0   -- plain FIFO, the behaviour before this pragma existed
**   10  -- the reference's default, and ours
**   100 -- strict LRU: every read relocates
**
** Figure 14 of the paper is a sweep of exactly this axis, which is why it is
** configuration and not a constant.  Expressed in percent rather than as a
** ratio because a PRAGMA value is parsed as an integer.
*/
void sqlite3PragmaBfCopyOnAccess(
  Parse *pParse,
  const char *zDb,
  const char *zValue
){
  Vdbe *v = sqlite3GetVdbe(pParse);

  if( zValue == 0 ){
    pParse->nMem = MAX(pParse->nMem, 1);
    sqlite3VdbeSetNumCols(v, 1);
    sqlite3VdbeAddOp2(v, OP_Integer, (int)(bfConfig.copyOnAccessRatio*100.0 + 0.5), 1);
    sqlite3VdbeSetColName(v, 0, COLNAME_NAME, "bf_copy_on_access", SQLITE_STATIC);
    sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 1);
  }else{
    int n = sqlite3Atoi(zValue);
    if( n < 0 ) n = 0;
    if( n > 100 ) n = 100;
    bfConfig.copyOnAccessRatio = (double)n / 100.0;
    /* Same reason as bf_cache_size and bf_min_record: the cache is created
    ** during the schema load, long before any BF pragma can run, so the global
    ** alone would never reach the ring this connection is already using.
    ** Unlike those two this needs no teardown -- the region is a threshold
    ** recomputed from the capacity, not a layout. */
    {
      sqlite3 *db = pParse->db;
      int iDb = sqlite3FindDbName(db, zDb);
      Btree *pBt = db->aDb[iDb>=0 ? iDb : 0].pBt;
      extern void sqlite3BfBtreeSetCopyOnAccess(Btree*, double);
      if( pBt ) sqlite3BfBtreeSetCopyOnAccess(pBt, bfConfig.copyOnAccessRatio);
    }
  }
}

#endif /* SQLITE_OMIT_BF_CACHE */
