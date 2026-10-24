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
#define BFWAL_VERSION   7             /* v3: CLEAR (2026-09-24); v4: rowid keys sign-
                                      ** flipped (2026-10-01); v5: logical ops, CLEAR
                                      ** replaced by MERGED (2026-10-18, C3); v6: CKPT
                                      ** (2026-10-18, C4b); v7: CKPT carries its
                                      ** segment's frame (C4d) */
#define BFWAL_HDRSIZE   12
#define BFWAL_OP_INSERT 0             /* == BFOP_INSERT */
#define BFWAL_OP_DELETE 1             /* == BFOP_DELETE (valLen==0) */
/* v5 (C3): "every op on a key in [lo, hi] of this table that precedes me in the
** log is in base pages -- except ops in frames (snap, start)".  key = lo||hi
** (two 8-byte encoded rowids), val = snap||start (two u32, big-endian); the
** page number is the merged leaf, a hint only.  Logged by a commit whose page
** images applied a mini-page's buffered records (a merge).  snap = the
** merging transaction's snapshot frame, start = the first frame of its own
** commit: ops other transactions committed after its snapshot (BEGIN
** CONCURRENT) are not in its page images.  Replaces CLEAR(leaf), whose leaf
** keying made every split a hazard. */
#define BFWAL_OP_MERGED 3
/* v6 (C4b): "the mini-page region now holds the state of every key as of
** this op" -- logged by a checkpoint's region commit, as its last record.  A
** connection whose index holds the whole log up to here (and the region it
** started from) folds it into base ops (sqlite3BfWalIndexRebase), which keeps
** the index -- and every scan of its markers -- bounded by the span since the
** last checkpoint.  pgno 1, root 2 (placeholders), no key; v7 (C4d): the
** value is the 4-byte frame whose state the region's new segment holds (its
** header's mxFrame), so a reader knows which segment its index now reflects. */
#define BFWAL_OP_CKPT   4

/* One logical record, as appended by the encoder or yielded by the iterator.
** On decode, pKey/pVal point INTO the source buffer (no copy). */
typedef struct BfWalRec BfWalRec;
struct BfWalRec {
  u32       pgno;      /* Target leaf page number (nonzero) */
  u32       rootPgno;  /* Owning rowid-table root page (greater than 1) */
  u8        op;      /* BFWAL_OP_INSERT, BFWAL_OP_DELETE or BFWAL_OP_MERGED */
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

/* A (table, key) reference and a key range (C3); also in bf_cache.h. */
#ifndef BF_KEYREF_DEFINED
#define BF_KEYREF_DEFINED
typedef struct BfKeyRef BfKeyRef;
struct BfKeyRef { u32 root; u8 aKey[8]; };
typedef struct BfKeyRange BfKeyRange;
struct BfKeyRange { u32 root; u8 lo[8]; u8 hi[8]; };
#endif

/* The replay state of one key: its LAST op unless a MERGED marker covers it.
** op = BFWAL_OP_INSERT / BFWAL_OP_DELETE, or -1 (no op, or covered: the base
** pages hold the key's value).  pVal points into the index. */
typedef struct BfKeyState BfKeyState;
struct BfKeyState {
  int       op;
  const u8 *pVal;
  u32       nVal;
  u32       iFrame;
  u32       hint;      /* the leaf the writer used (a hint) */
};

BfWalIndex *sqlite3BfWalIndexNew(void);
void        sqlite3BfWalIndexFree(BfWalIndex *p);
int         sqlite3BfWalIndexAppend(BfWalIndex *p, const BfWalRec *pRec,
                                    u32 iFrame);
/* Ops past mxFrame are ignored unless bKeepBeyond; markers past mxFrame are
** never honoured.  Returns 1 if the key has any op at all, else 0. */
int         sqlite3BfWalIndexKeyState(BfWalIndex *p, u32 root, const u8 *pKey,
                                      u32 mxFrame, int bKeepBeyond, BfKeyState*);
int         sqlite3BfWalIndexForEachKey(BfWalIndex *p,
                int (*xKey)(void *pCtx, u32 root, const u8 *pKey), void *pCtx);
int         sqlite3BfWalIndexKeyCount(BfWalIndex *p);

/* Feed every op of one record-batch frame payload into the index.  Returns
** BFWAL_OK, BFWAL_CORRUPT (bad payload), or BFWAL_NOMEM. */
int         sqlite3BfWalIndexAddFrame(BfWalIndex *p, const u8 *aBuf, int szBuf,
                                      u32 iFrame);

/* C4b: region (persisted mini-page) entries as BASE ops: older than every
** log op, covered by any MERGED marker over the key.  SetBase replaces the
** key's base op. */
int         sqlite3BfWalIndexHasBase(BfWalIndex *p, u32 root, const u8 *pKey);
int         sqlite3BfWalIndexSetBase(BfWalIndex *p, u32 root, const u8 *pKey,
                                     u8 op, const u8 *pVal, u32 nVal, u32 iFrame);
/* The state (as sqlite3BfWalIndexKeyState at mxFrame) of every key with a log
** op in frames (iAfter, mxFrame] -- or of every key, when bAll.  The last
** callback argument says whether the key has such an op. */
int         sqlite3BfWalIndexForEachState(BfWalIndex *p, u32 mxFrame, u32 iAfter,
                int bAll, int (*xState)(void*, u32, const u8*, const BfKeyState*, int),
                void *pCtx);
/* Every MERGED marker in frames (iAfter, mxFrame]: (root, lo, hi). */
int         sqlite3BfWalIndexForEachMarker(BfWalIndex *p, u32 mxFrame, u32 iAfter,
                int (*xMark)(void*, u32, const u8*, const u8*), void *pCtx);

/* C4b: every key's state at mxFrame becomes its base op; log ops and markers
** are dropped (a log restart, when the region holds exactly that state). */
void        sqlite3BfWalIndexRebase(BfWalIndex *p, u32 mxFrame, int bKeepFrames);
/* The frame of a CKPT op appended since the last call (0 if none); clears it. */
u32         sqlite3BfWalIndexTakeCkpt(BfWalIndex *p, u32 *piSeg);
/* C4d: drop the base op of every key for which xIn returns true. */
void        sqlite3BfWalIndexDropBase(BfWalIndex *p,
                int (*xIn)(void*, u32, const u8*), void *pCtx);

/* C4d: a log restart crossed by catch-up (sqlite3WalBfCrossGen). */
#ifndef BF_CROSSINFO_TYPEDEF
#define BF_CROSSINFO_TYPEDEF
typedef struct BfCrossInfo BfCrossInfo;
#endif
struct BfCrossInfo {
  u32 aOldSalt[2];     /* the generation the index was folded at ... */
  u32 iOldFrame;       /* ... and the frame */
  u32 nOldPage;        /* database size there */
  int bSegKnown;       /* the newest segment the base reflects, if known */
  u32 aSegSalt[2];
  u32 iSegMx;
  u32 aNewSalt[2];     /* the current generation */
  int bAll;            /* its frames were not read: its segments count too */
  u32 iTouchFrom;      /* its touch list starts here */
};

/* C4b: region entry codec.  kind = BFWAL_OP_INSERT, BFWAL_OP_DELETE, or
** BFREG_KILL (every older entry on a key in [key, hi] is dead). */
#define BFREG_KILL 3
/* C4d: a page written in the segment's interval of the log (root = the page
** number); readers that cross a log restart invalidate exactly these. */
#define BFREG_PAGE 5
typedef struct BfRegionEntry BfRegionEntry;
struct BfRegionEntry {
  u8 kind;
  u32 root;
  const u8 *pKey;      /* 8 bytes */
  const u8 *pHi;       /* KILL: 8 bytes */
  const u8 *pVal;      /* INSERT */
  u32 nVal;
  u32 iFrame;          /* INSERT/DELETE: the op's frame (see the codec) */
};
int  sqlite3BfRegionEntrySize(u8 kind, u32 nVal);
int  sqlite3BfRegionEncode(u8 *a, u8 kind, u32 root, const u8 *pKey,
                           const u8 *pVal, u32 nVal, u32 iFrame);
/* Bytes consumed, or 0 if aBuf[0..nAvail) does not start with a valid entry. */
int  sqlite3BfRegionDecode(const u8 *a, int nAvail, BfRegionEntry *pE);

/* Drop every op and marker carried by a frame after mxFrame (a rollback or
** savepoint undo discarded them, or they are a crashed commit's torn tail). */
void        sqlite3BfWalIndexPrune(BfWalIndex *p, u32 mxFrame);
int         sqlite3BfWalIndexForEachBeyond(BfWalIndex *p, u32 mxFrame,
                int (*xRec)(void*, const BfWalRec*), void *pCtx);

#endif /* !defined(SQLITE_OMIT_BF_CACHE) */
#endif /* SQLITE_BF_WAL_H */
