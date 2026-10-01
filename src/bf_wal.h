/*
** 2026 — BF-Tree v2, Phase 2 (physiological WAL).
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Record-batch frame codec for the BF-Tree physiological WAL.
**
** A physiological record frame reuses SQLite's ordinary WAL frame envelope
** (a 24-byte frame header + a szPage payload).  Where a normal frame's payload
** is a 4 KB page image, a *record-batch* frame's payload packs a list of
** rowid-table leaf mutations
**
**     [leafPgno u32][rootPgno u32][op u8][keyLen varint][valLen varint]
**     [key bytes][val bytes] ...
**
** so a commit persists small records instead of whole pages (§5.7 of the
** paper; see BF_TREE_V2_KNOWLEDGE.md §1.5).  This header declares the PURE
** codec: it encodes/decodes exactly one frame payload and knows nothing about
** the WAL file, the wal-index, cursors, or the mini-page cache.  Deliberately
** dependency-light (fixed-width ints only) so it is verifiable in isolation by
** the esbmc-verifier and libfuzzer-tester harnesses.
**
** Invariants the codec guarantees (the properties the harnesses assert):
**   - encode then decode is the identity over the appended record sequence;
**   - decode is bounds-safe on ANY input buffer (truncated, corrupt, hostile):
**     it returns BFWAL_CORRUPT/BFWAL_DONE and never reads past the buffer.
*/
#ifndef SQLITE_BF_WAL_H
#define SQLITE_BF_WAL_H
#ifndef SQLITE_OMIT_BF_CACHE

/* Return codes. */
#define BFWAL_OK       0    /* Success */
#define BFWAL_FULL     1    /* Append: record does not fit in the frame */
#define BFWAL_DONE     2    /* Iterator/index: no more records */
#define BFWAL_CORRUPT  3    /* Decode: malformed / out-of-bounds payload */
#define BFWAL_NOMEM    4    /* Index: allocation failed */

/* Batch-payload header: magic(4) version(2) nRec(2) nUsed(4) = 12 bytes.  op
** values reuse the mini-page BFOP_* space; only the dirty ops are ever logged. */
#define BFWAL_MAGIC     0x42465731u   /* "BFW1" */
#define BFWAL_VERSION   4             /* v3: BFWAL_OP_CLEAR (2026-09-24); v4: rowid
                                      ** keys sign-flipped (2026-10-01, bfEncodeRowid) */
#define BFWAL_HDRSIZE   12
#define BFWAL_OP_INSERT 0             /* == BFOP_INSERT */
#define BFWAL_OP_DELETE 1             /* == BFOP_DELETE (valLen==0) */
/* "Every earlier op of this leaf is now in base pages": logged by the commit
** whose page images applied the leaf's buffered records (a flush).  Carries no
** key and no value.  Replay starts each leaf after its last CLEAR -- the only
** sound way to order a record op against page images, because a flush writes
** a leaf's records wherever their keys now live, which after a split need not
** be that leaf at all. */
#define BFWAL_OP_CLEAR  2

/* One logical record, as appended by the encoder or yielded by the iterator.
** On decode, pKey/pVal point INTO the source buffer (no copy). */
typedef struct BfWalRec BfWalRec;
struct BfWalRec {
  u32       pgno;      /* Target leaf page number (nonzero) */
  u32       rootPgno;  /* Owning rowid-table root page (greater than 1) */
  u8        op;      /* BFWAL_OP_INSERT or BFWAL_OP_DELETE */
  u32       nKey;    /* Key length in bytes */
  u32       nVal;    /* Value length in bytes (0 for DELETE) */
  const u8 *pKey;    /* Key bytes */
  const u8 *pVal;    /* Value bytes (may be 0 when nVal==0) */
  u32       iFrame;  /* WAL frame that carried it: set by sqlite3BfWalIndexGet,
                     ** 0 from the iterator; ignored by the encoder */
};

/* Encoder over a caller-owned, page-sized buffer. */
typedef struct BfWalBatch BfWalBatch;
struct BfWalBatch {
  u8 *aBuf;          /* Destination frame payload (szBuf bytes) */
  int szBuf;         /* Payload size (== pager szPage) */
  int nUsed;         /* Bytes written so far (>= BFWAL_HDRSIZE) */
  int nRec;          /* Records appended so far */
};

/* Iterator over a decoded frame payload. */
typedef struct BfWalIter BfWalIter;
struct BfWalIter {
  const u8 *aBuf;    /* Source frame payload */
  int nUsed;         /* Valid byte count from the header (<= szBuf) */
  int nRec;          /* Record count from the header */
  int off;           /* Read cursor */
  int iRec;          /* Records yielded so far */
};

/* Encode. */
void sqlite3BfWalBatchInit(BfWalBatch *p, u8 *aBuf, int szBuf);
int  sqlite3BfWalBatchAppend(BfWalBatch *p, const BfWalRec *pRec);
void sqlite3BfWalBatchFinish(BfWalBatch *p);
int  sqlite3BfWalRecSize(const BfWalRec *pRec);   /* encoded byte cost */

/* Decode. */
int  sqlite3BfWalIterInit(BfWalIter *it, const u8 *aBuf, int szBuf);
int  sqlite3BfWalIterNext(BfWalIter *it, BfWalRec *pRec);

/* Cheap classification: does this payload look like a record batch?  Used by
** the reader/recovery walk to tell record frames from page-image frames when a
** frame-header marker is not otherwise available. */

/*
** In-memory page -> ordered record-ops index.
**
** As record frames are written (runtime) or scanned (recovery), their ops are
** appended here keyed by target pgno, preserving log order.  The pager consults
** it to reconstruct a page (latest page image + in-order replay of later record
** ops) and to apply record ops onto base pages at checkpoint.  Ops for a page
** are dropped once that page has been materialised/checkpointed.
**
** The index owns copies of the key/value bytes (the WAL frame buffer is not
** retained).  Opaque handle; storage details live in bf_wal.c.
*/
typedef struct BfWalIndex BfWalIndex;

BfWalIndex *sqlite3BfWalIndexNew(void);
void        sqlite3BfWalIndexFree(BfWalIndex *p);
int         sqlite3BfWalIndexAppend(BfWalIndex *p, const BfWalRec *pRec,
                                    u32 iFrame);
int         sqlite3BfWalIndexPageCount(BfWalIndex *p, u32 pgno);
int         sqlite3BfWalIndexGet(BfWalIndex *p, u32 pgno, int i, BfWalRec *pRec);

/* Feed every op of one record-batch frame payload into the index (recovery /
** reader walk convenience).  Returns BFWAL_OK, BFWAL_CORRUPT (bad payload), or
** BFWAL_NOMEM. */
int         sqlite3BfWalIndexAddFrame(BfWalIndex *p, const u8 *aBuf, int szBuf,
                                      u32 iFrame);

/* Drop every op carried by a frame after mxFrame.  Those frames are no longer
** part of the log -- a rollback or savepoint undo discarded them, or they are
** the torn tail of a crashed commit -- and their numbers will be reused by the
** next frames written, so an op left behind would later be ordered against the
** wrong page image.  Pages left with no ops keep their (empty) node. */
void        sqlite3BfWalIndexPrune(BfWalIndex *p, u32 mxFrame);

/* Visit each distinct target pgno held in the index (any order).  The callback
** may then walk that page's ops via PageCount/Get.  Iteration stops early if a
** callback returns non-BFWAL_OK, and that code is returned. */
int         sqlite3BfWalIndexForEachPage(BfWalIndex *p,
                int (*xPage)(void *pCtx, u32 pgno), void *pCtx);

#endif /* !defined(SQLITE_OMIT_BF_CACHE) */
#endif /* SQLITE_BF_WAL_H */
