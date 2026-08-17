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
** Configuration options:
**   SQLITE_CONFIG_BFCACHE        - Enable Bf-Tree cache globally
**   SQLITE_CONFIG_BFCACHE_SIZE   - Set circular buffer size
**
** Pragmas:
**   PRAGMA bf_cache              - Enable/disable for connection
**   PRAGMA bf_cache_size         - Get/set cache size
**   PRAGMA bf_cache_stats        - Show cache statistics
**   PRAGMA bf_promotion_rate     - Get/set read promotion rate
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
} BfConfig;

static BfConfig bfConfig = {
  1,                          /* Enabled by default */
  BF_DEFAULT_BUFFER_SIZE,     /* 32 MB default */
  BF_DEFAULT_PROMOTION_RATE,  /* 1% default */
  BF_DEFAULT_COPY_ON_ACCESS   /* 10% copy-on-access region */
};

/*
** Handle SQLITE_CONFIG_BFCACHE configuration.
*/
int sqlite3BfConfigSet(int op, va_list ap){
  int rc = SQLITE_OK;

  switch( op ){
    case SQLITE_CONFIG_BFCACHE: {
      /* Enable or disable Bf-Tree cache globally */
      int bEnable = va_arg(ap, int);
      bfConfig.bEnabled = bEnable ? 1 : 0;
      if( bEnable ){
        /* Set up Bf-Tree as the default pcache */
        sqlite3BfCacheSetMethods();
      }else{
        sqlite3BfResetGlobalCache();
        /* Revert to default pcache */
        sqlite3PCacheSetDefault();
      }
      break;
    }

    case SQLITE_CONFIG_BFCACHE_SIZE: {
      /* Set circular buffer size */
      i64 nSize = va_arg(ap, i64);
      if( nSize < BF_MAX_MINI_PAGE * 2 ){
        rc = SQLITE_ERROR;  /* Too small */
      }else{
        /* Round up to power of 2 */
        u64 n = 1;
        while( n < (u64)nSize ) n *= 2;
        bfConfig.nBufferSize = n;
      }
      break;
    }

    default:
      rc = SQLITE_ERROR;
      break;
  }

  return rc;
}

/*
** Handle SQLITE_CONFIG_GETBFCACHE configuration.
*/
int sqlite3BfConfigGet(int op, va_list ap){
  int rc = SQLITE_OK;

  switch( op ){
    case SQLITE_CONFIG_BFCACHE: {
      int *pEnable = va_arg(ap, int*);
      if( pEnable ) *pEnable = bfConfig.bEnabled;
      break;
    }

    case SQLITE_CONFIG_BFCACHE_SIZE: {
      i64 *pSize = va_arg(ap, i64*);
      if( pSize ) *pSize = (i64)bfConfig.nBufferSize;
      break;
    }

    default:
      rc = SQLITE_ERROR;
      break;
  }

  return rc;
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
** Get the promotion rate.
*/
int sqlite3BfCachePromotionRate(void){
  return bfConfig.nPromotionRate;
}

/*
** Set the promotion rate.
*/
void sqlite3BfCacheSetPromotionRate(int rate){
  if( rate < 0 ) rate = 0;
  if( rate > 100 ) rate = 100;
  bfConfig.nPromotionRate = rate;
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
      sqlite3BfResetGlobalCache();
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
      u64 nBufIns = 0, nFallback = 0;
      if( pBt ){
        extern void sqlite3BfBtreeInsertStats(Btree*, u64*, u64*);
        sqlite3BfBtreeInsertStats(pBt, &nBufIns, &nFallback);
      }
      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "buffered_inserts", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nBufIns, P4_INT64);
      sqlite3VdbeAddOp2(v, OP_ResultRow, 1, 2);

      sqlite3VdbeAddOp4(v, OP_String8, 0, 1, 0, "insert_fallbacks", P4_STATIC);
      sqlite3VdbeAddOp4Dup8(v, OP_Int64, 0, 2, 0,
                            (const u8*)&nFallback, P4_INT64);
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

#endif /* SQLITE_OMIT_BF_CACHE */
