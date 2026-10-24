/*
** bfbench -- YCSB-style benchmark driver for the BF-Tree SQLite fork.
**
** WHY THIS EXISTS.  The older harness (bench/read_bench.py) drives SQLite
** through the CLI: it builds one giant SQL script, and the "read benchmark" is
** a single JOIN.  That measures one query, not N operations, so it cannot
** report per-operation latency, it pays SQL text parsing in the measured
** region, and it cannot separate a warm phase from a measured phase.  This
** driver links the amalgamation directly, uses prepared statements, and times
** every individual operation.
**
** WHAT IT MEASURES.  For a workload mix over a rowid table it reports, for the
** MEASURED phase only (post-warmup deltas):
**
**   throughput          ops/s
**   latency             per op-type histogram -> p50/p90/p99/p99.9/p99.99/max
**   block I/O           /proc/self/io read_bytes / write_bytes.  These are
**                       counted at the block layer for THIS process and keep
**                       working inside a systemd --scope cgroup, unlike
**                       getrusage(RUSAGE_CHILDREN) which the old harness used
**                       and which silently read 0 under --memmax.
**   file growth         db / wal size deltas (write amplification)
**   engine counters     sqlite3_db_status cache hit/miss/write/spill, and every
**                       row of PRAGMA bf_cache_stats (absent on stock builds)
**   correctness         every read verifies the value it got back; a run that
**                       is fast because it is wrong reports read_errors > 0
**
** BUILD: see build_suts.sh.  Compiles against sqlite3.c + sqlite3.h.
**
** The schema is a YCSB "usertable" reduced to what v1 of the fork supports:
** a rowid table with one blob column.  Rowids are spaced `--key-spacing`
** apart at load time so the insert workload has gaps to land in, and so
** inserts split leaves that already hold data instead of appending.
*/
#define _GNU_SOURCE        /* O_DIRECT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <pthread.h>
#include "sqlite3.h"

/*----------------------------------------------------------------------------
** Small utilities
*/
static void die(const char *zFmt, ...){
  va_list ap;
  va_start(ap, zFmt);
  fprintf(stderr, "bfbench: ");
  vfprintf(stderr, zFmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(2);
}

static double nowSec(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t nowNs(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
** Hardware counters over the measured phase (2026-10-24).
**
** Time on this project's machines varies run to run (btrfs+LUKS laptop, cloud
** VMs); instruction and branch counts do not -- the same code on the same
** input retires the same instructions (bbtree's perf review measures +-0.0%).
** So next to device reads/op, instructions/op is the second machine-
** independent metric: the CPU cost of a design, separable from I/O luck.
**
** One perf_event_open group, user space only (exclude_kernel: works at
** perf_event_paranoid=2 and keeps syscall/IO-stack noise out), read once with
** time_enabled/time_running so multiplexing is visible.  The count includes
** the driver's own per-op work (key sampling, timing), identical for every
** SUT.  Many cloud VMs expose no PMU: then "available": 0 and the run goes
** on -- never fail a benchmark for want of a counter.
*/
#ifdef __linux__
# include <linux/perf_event.h>
# include <sys/syscall.h>
# include <sys/ioctl.h>
#endif
#define PMU_N 4           /* events per group */
#define PMU_MAXG 2        /* groups: one per core-type PMU on hybrid CPUs */
static const char *const azPmuName[PMU_N] = {
  "instructions", "cycles", "branches", "branch_misses"
};
/*
** Hybrid CPUs (Intel P+E cores) have one PMU per core type, and a plain
** PERF_TYPE_HARDWARE event counts only while the task runs on ONE of them: on
** the i5-1235U a single group ran for 22-28% of the task's time and two
** identical stock runs read 999 and 1278 instructions/op.  So open one group
** per core-type PMU (the PMU's type in config bits 32-63, Linux >= 5.13) and
** sum: each counts while the task is on its cores, together everything.
** Non-hybrid machines (cloud servers) get one plain group.  coverage =
** sum(time_running)/time_enabled is reported; below ~0.99 the counts are
** multiplexed samples, not totals, and must not be quoted.
*/
typedef struct Pmu Pmu;
struct Pmu {
  int nGroup;
  int fd[PMU_MAXG][PMU_N];  /* fd[g][0] leads group g; -1 if absent */
  int err;                  /* errno of the first leader's failure, else 0 */
  uint64_t aVal[PMU_N];     /* summed over groups */
  int aHave[PMU_N];         /* event opened in at least one group */
  uint64_t tEnabled, tRunning;   /* tEnabled: max over groups; tRunning: sum */
};
static int pmuSysType(const char *zPmu){
  char buf[256];
  FILE *f;
  int t = -1;
  snprintf(buf, sizeof(buf), "/sys/bus/event_source/devices/%s/type", zPmu);
  f = fopen(buf, "r");
  if( f ){ if( fscanf(f, "%d", &t)!=1 ) t = -1; fclose(f); }
  return t;
}
static void pmuOpen(Pmu *p){
  int g, i;
  memset(p, 0, sizeof(*p));
  for(g=0; g<PMU_MAXG; g++) for(i=0; i<PMU_N; i++) p->fd[g][i] = -1;
#ifdef __linux__
  {
    static const uint64_t aCfg[PMU_N] = {
      PERF_COUNT_HW_INSTRUCTIONS, PERF_COUNT_HW_CPU_CYCLES,
      PERF_COUNT_HW_BRANCH_INSTRUCTIONS, PERF_COUNT_HW_BRANCH_MISSES
    };
    int aType[PMU_MAXG], nType = 0;
    int tCore = pmuSysType("cpu_core"), tAtom = pmuSysType("cpu_atom");
    if( tCore>=0 ) aType[nType++] = tCore;
    if( tAtom>=0 ) aType[nType++] = tAtom;
    if( nType==0 ) aType[nType++] = -1;          /* plain events */
    for(g=0; g<nType; g++){
      for(i=0; i<PMU_N; i++){
        struct perf_event_attr a;
        memset(&a, 0, sizeof(a));
        a.size = sizeof(a);
        a.type = PERF_TYPE_HARDWARE;
        a.config = aCfg[i] | (aType[g]>=0 ? ((uint64_t)aType[g]<<32) : 0);
        a.disabled = (i==0);
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED
                      | PERF_FORMAT_TOTAL_TIME_RUNNING;
        p->fd[g][i] = (int)syscall(SYS_perf_event_open, &a, 0, -1,
                                   i==0 ? -1 : p->fd[g][0], 0);
        if( p->fd[g][i]<0 ){
          if( i==0 ){ if( !p->err ) p->err = errno; break; }
          continue;
        }
        p->aHave[i] = 1;
      }
      if( p->fd[g][0]>=0 ) p->nGroup = g+1;
    }
  }
#endif
}
static void pmuStart(Pmu *p){
#ifdef __linux__
  int g;
  for(g=0; g<PMU_MAXG; g++){
    if( p->fd[g][0]<0 ) continue;
    ioctl(p->fd[g][0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(p->fd[g][0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
  }
#else
  (void)p;
#endif
}
static void pmuStop(Pmu *p){
#ifdef __linux__
  int g, i, k;
  for(g=0; g<PMU_MAXG; g++){
    if( p->fd[g][0]>=0 ) ioctl(p->fd[g][0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
  }
  for(g=0; g<PMU_MAXG; g++){
    /* {nr, time_enabled, time_running, value[nr]} in open order; members
    ** that failed to open are absent from the group. */
    uint64_t buf[3+PMU_N];
    if( p->fd[g][0]<0 ) continue;
    if( read(p->fd[g][0], buf, sizeof(buf)) >= (ssize_t)(3*sizeof(uint64_t)) ){
      if( buf[1]>p->tEnabled ) p->tEnabled = buf[1];
      p->tRunning += buf[2];
      for(i=0, k=0; i<PMU_N && k<(int)buf[0]; i++){
        if( p->fd[g][i]>=0 ) p->aVal[i] += buf[3+k++];
      }
    }
    for(i=0; i<PMU_N; i++) if( p->fd[g][i]>=0 ) close(p->fd[g][i]);
  }
#else
  (void)p;
#endif
}
static void pmuJson(FILE *out, const Pmu *p, long long nOps){
  int i;
  if( p->nGroup==0 ){
    fprintf(out, "    \"pmu\": {\"available\": 0, \"error\": \"%s\"},\n",
            strerror(p->err ? p->err : ENOSYS));
    return;
  }
  fprintf(out, "    \"pmu\": {\"available\": 1, \"groups\": %d, "
               "\"time_enabled_ns\": %llu, \"time_running_ns\": %llu, "
               "\"coverage\": %.4f",
          p->nGroup, (unsigned long long)p->tEnabled,
          (unsigned long long)p->tRunning,
          p->tEnabled ? (double)p->tRunning/(double)p->tEnabled : 0.0);
  for(i=0; i<PMU_N; i++){
    if( !p->aHave[i] ){
      fprintf(out, ", \"%s\": null", azPmuName[i]);
    }else{
      fprintf(out, ", \"%s\": %llu, \"%s_per_op\": %.2f", azPmuName[i],
              (unsigned long long)p->aVal[i], azPmuName[i],
              nOps>0 ? (double)p->aVal[i]/(double)nOps : 0.0);
    }
  }
  fprintf(out, "},\n");
}

/* xoshiro256** -- fast, good quality, deterministic from a seed. */
typedef struct Rng { uint64_t s[4]; } Rng;

static uint64_t splitmix64(uint64_t *x){
  uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
static void rngSeed(Rng *r, uint64_t seed){
  uint64_t x = seed ? seed : 0x123456789abcdefull;
  int i;
  for(i=0; i<4; i++) r->s[i] = splitmix64(&x);
}
static uint64_t rngNext(Rng *r){
  uint64_t *s = r->s;
  uint64_t result = s[1] * 5;
  result = ((result << 7) | (result >> 57)) * 9;
  uint64_t t = s[1] << 17;
  s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
  s[3] = (s[3] << 45) | (s[3] >> 19);
  return result;
}
static double rngDouble(Rng *r){          /* uniform in [0,1) */
  return (rngNext(r) >> 11) * (1.0 / 9007199254740992.0);
}

/* FNV-1a 64.  YCSB scrambles the Zipf rank through this so the hot set is
** scattered over the key space instead of being the first N (physically
** adjacent) rows -- otherwise "skew" degenerates into "read the first few
** pages", which any page cache wins trivially. */
static uint64_t fnv64(uint64_t v){
  uint64_t h = 0xcbf29ce484222325ull;
  int i;
  for(i=0; i<8; i++){
    h ^= (v >> (i*8)) & 0xff;
    h *= 0x100000001b3ull;
  }
  return h;
}

/*----------------------------------------------------------------------------
** Key distributions
*/
#define DIST_UNIFORM 0
#define DIST_ZIPF    1
#define DIST_LATEST  2
#define DIST_ZIPFRAW 3

typedef struct Zipf {
  uint64_t n;          /* item count */
  double theta;
  double zetan, zeta2, alpha, eta;
} Zipf;

static double zetaSum(uint64_t n, double theta){
  double sum = 0.0;
  uint64_t i;
  for(i=0; i<n; i++) sum += 1.0 / pow((double)(i+1), theta);
  return sum;
}

/* YCSB ZipfianGenerator (Gray et al. "Quickly generating billion-record
** synthetic databases", the same formulation the Bf-Tree harness uses). */
static void zipfInit(Zipf *z, uint64_t n, double theta){
  z->n = n;
  z->theta = theta;
  z->zetan = zetaSum(n, theta);
  z->zeta2 = zetaSum(2, theta);
  z->alpha = 1.0 / (1.0 - theta);
  z->eta = (1.0 - pow(2.0/(double)n, 1.0 - theta))
         / (1.0 - z->zeta2 / z->zetan);
}
static uint64_t zipfNext(Zipf *z, Rng *r){
  double u = rngDouble(r);
  double uz = u * z->zetan;
  if( uz < 1.0 ) return 0;
  if( uz < 1.0 + pow(0.5, z->theta) ) return 1;
  {
    double v = z->eta*u - z->eta + 1.0;
    uint64_t ret = (uint64_t)((double)z->n * pow(v, z->alpha));
    return ret >= z->n ? z->n - 1 : ret;
  }
}

/*----------------------------------------------------------------------------
** Insert key allocation.
**
** Inserts must (a) never collide with an existing row -- a duplicate would
** turn an INSERT into a constraint error and quietly stop measuring the insert
** path -- and (b) land scattered across the whole key space, so each commit
** touches a different leaf.  Tracking used keys in a hash set is impossible at
** 60M rows, so instead we PERMUTE the gap slots: slot s is visited exactly once
** because the permutation is a bijection.
**
** The permutation is a 4-round Feistel network over the smallest power of two
** >= domain, with cycle-walking to stay inside the domain.  Bijective by
** construction, so uniqueness is guaranteed without any bookkeeping.
*/
typedef struct Feistel {
  uint64_t domain;     /* number of slots */
  int halfBits;        /* bits per half */
  uint64_t mask;       /* (1<<halfBits)-1 */
  uint64_t key[4];
} Feistel;

static void feistelInit(Feistel *f, uint64_t domain, uint64_t seed){
  int bits = 1;
  uint64_t x = seed ^ 0xa5a5a5a5deadbeefull;
  int i;
  while( bits < 62 && ((uint64_t)1 << bits) < domain ) bits++;
  if( bits & 1 ) bits++;                       /* even, so halves are equal */
  f->domain = domain ? domain : 1;
  f->halfBits = bits / 2;
  f->mask = ((uint64_t)1 << f->halfBits) - 1;
  for(i=0; i<4; i++) f->key[i] = splitmix64(&x);
}
static uint64_t feistelAt(Feistel *f, uint64_t idx){
  uint64_t v = idx % f->domain;
  int guard = 0;
  do {
    uint64_t l = (v >> f->halfBits) & f->mask;
    uint64_t r = v & f->mask;
    int i;
    for(i=0; i<4; i++){
      uint64_t nl = r;
      uint64_t fr = (fnv64(r ^ f->key[i]) >> 13) & f->mask;
      r = l ^ fr;
      l = nl;
    }
    v = (l << f->halfBits) | r;
    /* cycle-walk: keep applying the permutation until we land in range */
  } while( v >= f->domain && ++guard < 64 );
  return v < f->domain ? v : (idx % f->domain);
}

/*----------------------------------------------------------------------------
** Latency histogram.  HDR-style: 7 significant bits, so ~0.8% relative error,
** 8192 counters.  Values are nanoseconds.
*/
#define HIST_SUB_BITS 7
#define HIST_SUB      (1 << HIST_SUB_BITS)
#define HIST_SIZE     8192

typedef struct Hist {
  uint64_t count;
  uint64_t total;      /* sum of values, for the mean */
  uint64_t max;
  uint64_t bucket[HIST_SIZE];
} Hist;

static int histIndex(uint64_t v){
  int e, idx;
  if( v < HIST_SUB ) return (int)v;
  e = 63 - __builtin_clzll(v);
  idx = ((e - (HIST_SUB_BITS-1)) << HIST_SUB_BITS)
      | (int)((v >> (e - HIST_SUB_BITS)) & (HIST_SUB - 1));
  return idx < HIST_SIZE ? idx : HIST_SIZE - 1;
}
static uint64_t histValue(int idx){
  int e;
  if( idx < HIST_SUB ) return (uint64_t)idx;
  e = (idx >> HIST_SUB_BITS) + (HIST_SUB_BITS - 1);
  return ((uint64_t)(idx & (HIST_SUB-1)) | HIST_SUB) << (e - HIST_SUB_BITS);
}
static void histAdd(Hist *h, uint64_t v){
  h->bucket[histIndex(v)]++;
  h->count++;
  h->total += v;
  if( v > h->max ) h->max = v;
}
static uint64_t histPct(Hist *h, double pct){
  uint64_t want, seen = 0;
  int i;
  if( h->count == 0 ) return 0;
  want = (uint64_t)(h->count * pct / 100.0);
  if( want >= h->count ) want = h->count - 1;
  for(i=0; i<HIST_SIZE; i++){
    seen += h->bucket[i];
    if( seen > want ) return histValue(i);
  }
  return h->max;
}

/*----------------------------------------------------------------------------
** /proc/self/io -- block-layer byte counters for THIS process.
**
** read_bytes/write_bytes are what the process actually caused to be fetched
** from / sent to the block device, which is the hardware-independent number
** the paper cares about.  rchar/wchar are logical (they include page-cache
** hits), so both are reported: rchar-vs-read_bytes is the cache hit story.
*/
typedef struct ProcIo {
  uint64_t rchar, wchar, syscr, syscw, read_bytes, write_bytes, cancelled;
} ProcIo;

static void procIoRead(ProcIo *io){
  FILE *f = fopen("/proc/self/io", "r");
  char line[256];
  memset(io, 0, sizeof(*io));
  if( f==0 ) return;
  while( fgets(line, sizeof(line), f) ){
    unsigned long long v;
    if( sscanf(line, "rchar: %llu", &v)==1 )                  io->rchar = v;
    else if( sscanf(line, "wchar: %llu", &v)==1 )             io->wchar = v;
    else if( sscanf(line, "syscr: %llu", &v)==1 )             io->syscr = v;
    else if( sscanf(line, "syscw: %llu", &v)==1 )             io->syscw = v;
    else if( sscanf(line, "read_bytes: %llu", &v)==1 )        io->read_bytes = v;
    else if( sscanf(line, "write_bytes: %llu", &v)==1 )       io->write_bytes = v;
    else if( sscanf(line, "cancelled_write_bytes: %llu",&v)==1) io->cancelled = v;
  }
  fclose(f);
}

static long long fileSize(const char *zPath){
  struct stat st;
  if( stat(zPath, &st)!=0 ) return 0;
  return (long long)st.st_size;
}

/* Evict a file's clean pages from the OS page cache.  Needed because cgroup v2
** charges a page-cache page to whoever faulted it in FIRST: pages left over
** from the load phase remain usable by the capped process for free and quietly
** defeat MemoryMax. */
static void dropFileCache(const char *zDb){
  const char *azSuffix[] = { "", "-wal", "-shm" };
  char buf[4096];
  int i;
  for(i=0; i<3; i++){
    int fd;
    snprintf(buf, sizeof(buf), "%s%s", zDb, azSuffix[i]);
    fd = open(buf, O_RDONLY);
    if( fd < 0 ) continue;
    fsync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
  }
}


/*----------------------------------------------------------------------------
** Direct-I/O measurement VFS ("bfdio", --direct-io; plan item H7).
**
** WHY.  Under buffered I/O a record-cache hit that avoids a page read usually
** avoids only an OS page-cache hit -- a memcpy, not a device read -- so BF read
** 17-29% fewer block bytes than stock and converted almost none of it into
** throughput.  ../bf-tree measures with direct I/O so that a miss costs a device
** read.  This shim does the same for BOTH SUTs, in the harness: SQLite core and
** its unix VFS are untouched, so the comparison is still against stock SQLite.
**
** HOW.  A shim over the default VFS.  Every file is the real unix file; for the
** MAIN DATABASE only, the shim also opens a second descriptor with O_DIRECT and
** serves xRead -- and page-aligned xWrite -- through it, via an aligned bounce
** buffer (pcache buffers are not block-aligned, and the 100-byte header read is
** neither aligned nor block-sized).  Locking, sync, size, truncate and shm stay
** on the unix file; fsync on either descriptor syncs the same inode.  Shm and
** journals stay buffered.  The kernel keeps O_DIRECT coherent with the
** page cache (it writes back and invalidates the range first), so a buffered
** write elsewhere cannot make a direct read stale.
**
** THE WAL TOO (2026-10-12).  Until then the WAL stayed buffered, so a write
** campaign compared two engines whose WAL reads were OS page-cache hits (with
** autocheckpoint 0, stock re-read its pages from the WAL at 0.011 device reads
** per op) and whose WAL writes were deferred page-cache writeback.  Now the WAL
** gets its own O_DIRECT descriptor.  Its offsets are not block-aligned (32-byte
** header, frames of 24 + page size), so:
**   - reads are aligned covering reads through the bounce buffer, like the db's;
**   - writes are read-modify-write of whole blocks.  The bytes of a partial edge
**     block outside the write MUST be preserved (SQLite rewrites a frame of the
**     open transaction in place, and the next frame shares its block), so they
**     come from a TAIL-BLOCK CACHE -- the last block written, shared by every
**     connection on the same WAL in this process, under a mutex -- or, on a
**     miss, from a device read counted separately as wal_rmw_reads.  Appends
**     always hit (each write's head block is the previous write's tail), so a
**     commit costs device WRITES only, as on the real thing.
**   - WRITE-COMBINING.  SQLite writes a frame as two calls (24-byte header,
**     then the page), so writing whole blocks per call put each block on the
**     device ~3 times (device/logical bytes 3.0 measured), where buffered I/O
**     writes each page once at writeback.  Complete blocks go to the device at
**     once; the trailing PARTIAL block stays in the tail cache (dirty) until it
**     completes or until a sync, an overlapping read, xFileSize, truncate or
**     close.  So each block reaches the device about once, as with the page
**     cache.  Like unsynced buffered data, an unsynced partial block is lost if
**     the PROCESS dies (synchronous=NORMAL semantics); benchmarks do not die.
**   - writing whole blocks can leave the file up to one block longer than
**     SQLite wrote; the extra bytes are zeros, which no frame checksum accepts,
**     so recovery stops exactly where it would have.
** Separate counters: wal_reads / wal_read_bytes (real reads), wal_rmw_reads,
** wal_writes / wal_write_bytes (device bytes) and wal_logical_bytes (what
** SQLite asked to write).  Multi-PROCESS writers are not supported (the tail
** cache is per process); the harness is one process.
**
** A filesystem that rejects O_DIRECT fails the run: falling back silently would
** measure buffered I/O under a direct-I/O label.  One that ACCEPTS it but serves
** from the page cache anyway (btrfs does, for compressed extents) is caught by
** the per-run proof in the JSON: db_resident_pages before/after (mincore) must
** not grow, and read_bytes should track pcache misses x page size.
*/
#define DIO_ALIGN 4096

/* The WAL's tail-block cache: one per WAL path, shared by all connections. */
typedef struct DioTail {
  char zPath[512];
  pthread_mutex_t mu;
  int bValid;
  sqlite3_int64 iBlock;       /* file offset of the cached block */
  unsigned char *aBlk;        /* DIO_ALIGN bytes, aligned */
  sqlite3_int64 iHigh;        /* end of what THIS log generation has written;
                              ** -1 = unknown (file opened, not yet written) */
  int bDirty;                 /* aBlk holds bytes not yet on the device */
} DioTail;
#define DIO_MAX_TAIL 8
static DioTail gDioTail[DIO_MAX_TAIL];
static int gnDioTail = 0;
static pthread_mutex_t gDioTailMu = PTHREAD_MUTEX_INITIALIZER;

typedef struct DioFile {
  sqlite3_file base;          /* .pMethods = &dioMethods (or the real ones) */
  sqlite3_file *pReal;        /* the unix file, allocated right after this */
  int fd;                     /* O_DIRECT descriptor; -1 when not db or WAL */
  int bWal;                   /* this is a WAL file */
  DioTail *pTail;             /* WAL only */
  unsigned char *aBuf;        /* DIO_ALIGN-aligned bounce buffer */
  size_t nBuf;
} DioFile;

static sqlite3_vfs *gDioReal = 0;
static struct {
  unsigned long long nRead, nReadBytes, nReadUnaligned, nShort;
  unsigned long long nWrite, nWriteBytes, nWriteFallback;
  unsigned long long nWalRead, nWalReadBytes, nWalRmw, nWalShort;
  unsigned long long nWalWrite, nWalWriteBytes, nWalLogicalBytes;
  unsigned long long nWalTailFlush;
} gDio;

/* Write the pending partial tail block (write-combining, see the header). */
static int dioTailFlush(DioTail *t, int fd){
  ssize_t put;
  if( !t->bDirty ) return SQLITE_OK;
  do{ put = pwrite(fd, t->aBlk, DIO_ALIGN, t->iBlock); }while( put<0 && errno==EINTR );
  if( put!=DIO_ALIGN ) return (put<0 && errno==ENOSPC) ? SQLITE_FULL : SQLITE_IOERR_WRITE;
  t->bDirty = 0;
  gDio.nWalTailFlush++;
  gDio.nWalWrite++;
  gDio.nWalWriteBytes += DIO_ALIGN;
  return SQLITE_OK;
}
static int dioTailFlushLocked(DioFile *f){
  int rc;
  if( !f->pTail ) return SQLITE_OK;
  pthread_mutex_lock(&f->pTail->mu);
  rc = dioTailFlush(f->pTail, f->fd);
  pthread_mutex_unlock(&f->pTail->mu);
  return rc;
}

static DioTail *dioTailFor(const char *zPath){
  DioTail *t = 0;
  int i;
  pthread_mutex_lock(&gDioTailMu);
  for(i=0; i<gnDioTail; i++){
    if( strcmp(gDioTail[i].zPath, zPath)==0 ){ t = &gDioTail[i]; break; }
  }
  if( t==0 ){
    if( gnDioTail>=DIO_MAX_TAIL ) die("--direct-io: too many WAL files");
    t = &gDioTail[gnDioTail++];
    snprintf(t->zPath, sizeof(t->zPath), "%s", zPath);
    pthread_mutex_init(&t->mu, 0);
    if( posix_memalign((void**)&t->aBlk, DIO_ALIGN, DIO_ALIGN)!=0 ) die("out of memory");
    t->bValid = 0;
    t->iHigh = -1;
  }
  pthread_mutex_unlock(&gDioTailMu);
  return t;
}

static int dioBuf(DioFile *f, size_t n){
  if( n<=f->nBuf ) return 1;
  free(f->aBuf);
  f->aBuf = 0; f->nBuf = 0;
  if( posix_memalign((void**)&f->aBuf, DIO_ALIGN, n)!=0 ){ f->aBuf = 0; return 0; }
  f->nBuf = n;
  return 1;
}

static int dioTailFlushLocked(DioFile *f);
static int dioClose(sqlite3_file *pFile){
  DioFile *f = (DioFile*)pFile;
  int rc;
  if( f->fd>=0 ) dioTailFlushLocked(f);    /* before the descriptor goes */
  rc = f->pReal->pMethods ? f->pReal->pMethods->xClose(f->pReal) : SQLITE_OK;
  if( f->fd>=0 ) close(f->fd);
  free(f->aBuf);
  f->fd = -1; f->aBuf = 0; f->nBuf = 0;
  return rc;
}

static int dioRead(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite3_int64 iOfst){
  DioFile *f = (DioFile*)pFile;
  sqlite3_int64 lo = iOfst & ~(sqlite3_int64)(DIO_ALIGN-1);
  sqlite3_int64 hi = (iOfst + iAmt + DIO_ALIGN - 1) & ~(sqlite3_int64)(DIO_ALIGN-1);
  size_t n = (size_t)(hi - lo);
  ssize_t got;
  sqlite3_int64 avail;
  if( f->fd<0 ) return f->pReal->pMethods->xRead(f->pReal, zBuf, iAmt, iOfst);
  if( f->pTail ){                       /* WAL: pending tail must be readable */
    int rcF = SQLITE_OK;
    pthread_mutex_lock(&f->pTail->mu);
    if( f->pTail->bDirty && f->pTail->iBlock>=lo && f->pTail->iBlock<hi ){
      rcF = dioTailFlush(f->pTail, f->fd);
    }
    pthread_mutex_unlock(&f->pTail->mu);
    if( rcF!=SQLITE_OK ) return rcF;
  }
  if( !dioBuf(f, n) ) return SQLITE_IOERR_NOMEM;
  if( !f->bWal && (lo!=iOfst || (iAmt & (DIO_ALIGN-1))!=0) ) gDio.nReadUnaligned++;
  do{ got = pread(f->fd, f->aBuf, n, lo); }while( got<0 && errno==EINTR );
  if( got<0 ){
    if( errno==EINVAL ) die("--direct-io: O_DIRECT read rejected (EINVAL) -- "
                            "filesystem or alignment does not support it");
    return SQLITE_IOERR_READ;
  }
  if( f->bWal ){
    gDio.nWalRead++;
    gDio.nWalReadBytes += (unsigned long long)got;
  }else{
    gDio.nRead++;
    gDio.nReadBytes += (unsigned long long)got;
  }
  avail = (sqlite3_int64)got - (iOfst - lo);
  if( avail >= iAmt ){
    memcpy(zBuf, f->aBuf + (iOfst - lo), iAmt);
    return SQLITE_OK;
  }
  /* Short read past end of file: SQLite's contract is to zero-fill the rest
  ** and say SQLITE_IOERR_SHORT_READ (os_unix.c unixRead does the same). */
  if( avail < 0 ) avail = 0;
  if( avail ) memcpy(zBuf, f->aBuf + (iOfst - lo), (size_t)avail);
  memset((char*)zBuf + avail, 0, (size_t)(iAmt - avail));
  if( f->bWal ) gDio.nWalShort++; else gDio.nShort++;
  return SQLITE_IOERR_SHORT_READ;
}

/* Fill dst (DIO_ALIGN bytes) with the current content of the block at iBlk:
** the tail cache if it holds it, zeros where nothing live can be (past end of
** file, or wholly past what this log generation has written), else a device
** read.  After a checkpoint restarts the log, new frames overwrite the old
** generation's bytes; those are dead (wrong salt, never accepted), so reading
** them back to "preserve" them was a device read per frame for nothing. */
static int dioWalFill(DioFile *f, sqlite3_int64 iBlk, unsigned char *dst){
  DioTail *t = f->pTail;
  struct stat st;
  ssize_t got;
  if( t->bValid && t->iBlock==iBlk ){
    memcpy(dst, t->aBlk, DIO_ALIGN);
    return SQLITE_OK;
  }
  if( t->iHigh>=0 && iBlk>=t->iHigh ){
    memset(dst, 0, DIO_ALIGN);
    return SQLITE_OK;
  }
  if( fstat(f->fd, &st)==0 && iBlk>=(sqlite3_int64)st.st_size ){
    memset(dst, 0, DIO_ALIGN);
    return SQLITE_OK;
  }
  do{ got = pread(f->fd, dst, DIO_ALIGN, iBlk); }while( got<0 && errno==EINTR );
  if( got<0 ) return SQLITE_IOERR_READ;
  if( got<DIO_ALIGN ) memset(dst + got, 0, (size_t)(DIO_ALIGN - got));
  gDio.nWalRmw++;
  return SQLITE_OK;
}

/* WAL write: read-modify-write of whole blocks (see the header comment). */
static int dioWalWrite(DioFile *f, const void *zBuf, int iAmt, sqlite3_int64 iOfst){
  DioTail *t = f->pTail;
  sqlite3_int64 lo = iOfst & ~(sqlite3_int64)(DIO_ALIGN-1);
  sqlite3_int64 end = iOfst + iAmt;
  sqlite3_int64 hi = (end + DIO_ALIGN - 1) & ~(sqlite3_int64)(DIO_ALIGN-1);
  size_t n = (size_t)(hi - lo);
  ssize_t put;
  int rc = SQLITE_OK;
  if( !dioBuf(f, n) ) return SQLITE_IOERR_NOMEM;
  pthread_mutex_lock(&t->mu);
  /* A pending tail block that this write neither starts nor ends in must reach
  ** the device first: the cache holds only one block. */
  if( t->bDirty && t->iBlock!=lo && t->iBlock!=hi - DIO_ALIGN ){
    rc = dioTailFlush(t, f->fd);
    if( rc!=SQLITE_OK ){ pthread_mutex_unlock(&t->mu); return rc; }
  }
  /* Offset 0 is the WAL header: it is written exactly when a log generation
  ** starts (first frame after a restart, or a new log), so nothing beyond it
  ** is live any more. */
  if( iOfst==0 ) t->iHigh = 0;
  if( iOfst!=lo ) rc = dioWalFill(f, lo, f->aBuf);
  if( rc==SQLITE_OK && end!=hi && (hi - DIO_ALIGN!=lo || iOfst==lo) ){
    if( t->iHigh>=0 && end>=t->iHigh
     && !(t->bValid && t->iBlock==hi - DIO_ALIGN) ){
      /* Everything after this write in its last block is past the
      ** generation's high-water mark: dead bytes, zero them. */
      memset(f->aBuf + (n - DIO_ALIGN), 0, DIO_ALIGN);
    }else{
      rc = dioWalFill(f, hi - DIO_ALIGN, f->aBuf + (n - DIO_ALIGN));
    }
  }
  if( rc==SQLITE_OK ){
    /* Complete blocks go now; a partial last block stays pending. */
    size_t nNow = (end==hi) ? n : n - DIO_ALIGN;
    memcpy(f->aBuf + (iOfst - lo), zBuf, (size_t)iAmt);
    put = (ssize_t)nNow;
    if( nNow>0 ){
      do{ put = pwrite(f->fd, f->aBuf, nNow, lo); }while( put<0 && errno==EINTR );
      if( put<0 && errno==EINVAL ) die("--direct-io: O_DIRECT WAL write rejected (EINVAL)");
    }
    if( put!=(ssize_t)nNow ){
      rc = (put<0 && errno==ENOSPC) ? SQLITE_FULL : SQLITE_IOERR_WRITE;
      t->bValid = 0;
      t->bDirty = 0;
    }else{
      memcpy(t->aBlk, f->aBuf + (n - DIO_ALIGN), DIO_ALIGN);
      t->iBlock = hi - DIO_ALIGN;
      t->bValid = 1;
      t->bDirty = (end!=hi);
      if( t->iHigh>=0 && end>t->iHigh ) t->iHigh = end;
      if( nNow>0 ){
        gDio.nWalWrite++;
        gDio.nWalWriteBytes += (unsigned long long)nNow;
      }
      gDio.nWalLogicalBytes += (unsigned long long)iAmt;
    }
  }
  pthread_mutex_unlock(&t->mu);
  return rc;
}

static int dioWrite(sqlite3_file *pFile, const void *zBuf, int iAmt,
                    sqlite3_int64 iOfst){
  DioFile *f = (DioFile*)pFile;
  ssize_t put;
  if( f->fd<0 ) return f->pReal->pMethods->xWrite(f->pReal, zBuf, iAmt, iOfst);
  if( f->bWal ) return dioWalWrite(f, zBuf, iAmt, iOfst);
  if( (iOfst & (DIO_ALIGN-1))!=0 || (iAmt & (DIO_ALIGN-1))!=0 ){
    gDio.nWriteFallback++;      /* buffered; the kernel keeps them coherent */
    return f->pReal->pMethods->xWrite(f->pReal, zBuf, iAmt, iOfst);
  }
  if( !dioBuf(f, (size_t)iAmt) ) return SQLITE_IOERR_NOMEM;
  memcpy(f->aBuf, zBuf, iAmt);
  do{ put = pwrite(f->fd, f->aBuf, (size_t)iAmt, iOfst); }while( put<0 && errno==EINTR );
  if( put<0 && errno==EINVAL ) die("--direct-io: O_DIRECT write rejected (EINVAL)");
  if( put!=iAmt ) return put<0 && errno==ENOSPC ? SQLITE_FULL : SQLITE_IOERR_WRITE;
  gDio.nWrite++;
  gDio.nWriteBytes += (unsigned long long)iAmt;
  return SQLITE_OK;
}

/* Everything else is the unix file's own behaviour. */
#define DIO_REAL(f) (((DioFile*)(f))->pReal)
static int dioTruncate(sqlite3_file *p, sqlite3_int64 n){
  DioFile *f = (DioFile*)p;
  if( f->pTail ){                       /* the cached block may be cut off */
    pthread_mutex_lock(&f->pTail->mu);
    if( f->pTail->bDirty && f->pTail->iBlock<n ) dioTailFlush(f->pTail, f->fd);
    f->pTail->bDirty = 0;
    f->pTail->bValid = 0;
    pthread_mutex_unlock(&f->pTail->mu);
  }
  return DIO_REAL(p)->pMethods->xTruncate(DIO_REAL(p), n);
}
static int dioSync(sqlite3_file *p, int fl){
  int rc = dioTailFlushLocked((DioFile*)p);
  if( rc!=SQLITE_OK ) return rc;
  return DIO_REAL(p)->pMethods->xSync(DIO_REAL(p), fl);
}
static int dioFileSize(sqlite3_file *p, sqlite3_int64 *pn){
  int rc = dioTailFlushLocked((DioFile*)p);
  if( rc!=SQLITE_OK ) return rc;
  return DIO_REAL(p)->pMethods->xFileSize(DIO_REAL(p), pn);
}
static int dioLock(sqlite3_file *p, int e){ return DIO_REAL(p)->pMethods->xLock(DIO_REAL(p), e); }
static int dioUnlock(sqlite3_file *p, int e){ return DIO_REAL(p)->pMethods->xUnlock(DIO_REAL(p), e); }
static int dioCheckReserved(sqlite3_file *p, int *pr){ return DIO_REAL(p)->pMethods->xCheckReservedLock(DIO_REAL(p), pr); }
static int dioFileControl(sqlite3_file *p, int op, void *a){ return DIO_REAL(p)->pMethods->xFileControl(DIO_REAL(p), op, a); }
static int dioSectorSize(sqlite3_file *p){ return DIO_REAL(p)->pMethods->xSectorSize(DIO_REAL(p)); }
static int dioDevChar(sqlite3_file *p){ return DIO_REAL(p)->pMethods->xDeviceCharacteristics(DIO_REAL(p)); }
static int dioShmMap(sqlite3_file *p, int i, int sz, int ext, void volatile **pp){ return DIO_REAL(p)->pMethods->xShmMap(DIO_REAL(p), i, sz, ext, pp); }
static int dioShmLock(sqlite3_file *p, int o, int n, int fl){ return DIO_REAL(p)->pMethods->xShmLock(DIO_REAL(p), o, n, fl); }
static void dioShmBarrier(sqlite3_file *p){ DIO_REAL(p)->pMethods->xShmBarrier(DIO_REAL(p)); }
static int dioShmUnmap(sqlite3_file *p, int del){ return DIO_REAL(p)->pMethods->xShmUnmap(DIO_REAL(p), del); }
/* mmap would bypass xRead entirely: refuse it (cmdRun also asserts mmap_size=0). */
static int dioFetch(sqlite3_file *p, sqlite3_int64 o, int n, void **pp){ (void)p; (void)o; (void)n; *pp = 0; return SQLITE_OK; }
static int dioUnfetch(sqlite3_file *p, sqlite3_int64 o, void *pv){ (void)p; (void)o; (void)pv; return SQLITE_OK; }
/* (An xFetch that hands back no mapping makes SQLite fall back to xRead, for
** every file.  Only the main db is ever mmapped, and cmdRun sets mmap_size=0.) */

static const sqlite3_io_methods dioMethods = {
  3, dioClose, dioRead, dioWrite, dioTruncate, dioSync, dioFileSize,
  dioLock, dioUnlock, dioCheckReserved, dioFileControl, dioSectorSize,
  dioDevChar, dioShmMap, dioShmLock, dioShmBarrier, dioShmUnmap,
  dioFetch, dioUnfetch
};

static int dioOpen(sqlite3_vfs *pVfs, sqlite3_filename zName, sqlite3_file *pFile,
                   int flags, int *pOutFlags){
  DioFile *f = (DioFile*)pFile;
  int rc;
  (void)pVfs;
  memset(f, 0, sizeof(*f));
  f->fd = -1;
  f->pReal = (sqlite3_file*)&f[1];
  rc = gDioReal->xOpen(gDioReal, zName, f->pReal, flags, pOutFlags);
  if( rc!=SQLITE_OK ){
    f->pReal->pMethods = 0;
    return rc;
  }
  f->base.pMethods = &dioMethods;
  if( (flags & (SQLITE_OPEN_MAIN_DB|SQLITE_OPEN_WAL))==0 || zName==0 ){
    return SQLITE_OK;           /* not db or WAL: fd<0, every call forwards */
  }
  if( flags & SQLITE_OPEN_WAL ){
    f->bWal = 1;
    f->pTail = dioTailFor(zName);
    /* Another connection may have written since this one last looked. */
    pthread_mutex_lock(&f->pTail->mu);
    f->pTail->bValid = 0;
    pthread_mutex_unlock(&f->pTail->mu);
  }
  f->fd = open(zName, ((flags & SQLITE_OPEN_READONLY) ? O_RDONLY : O_RDWR)
                      | O_DIRECT | O_CLOEXEC);
  if( f->fd<0 ){
    die("--direct-io: open(%s, O_DIRECT) failed: %s", zName, strerror(errno));
  }
  return SQLITE_OK;
}

static sqlite3_vfs gDioVfs;
static void dioRegister(void){
  gDioReal = sqlite3_vfs_find(0);
  if( gDioReal==0 ) die("--direct-io: no default VFS");
  gDioVfs = *gDioReal;
  gDioVfs.zName = "bfdio";
  gDioVfs.szOsFile = (int)sizeof(DioFile) + gDioReal->szOsFile;
  gDioVfs.xOpen = dioOpen;
  gDioVfs.pNext = 0;
  if( sqlite3_vfs_register(&gDioVfs, 0)!=SQLITE_OK ) die("--direct-io: register failed");
}

/* Resident OS page-cache pages of a file (mincore over a PROT_READ mapping,
** which faults nothing in).  -1 when it cannot be measured. */
static long long residentPages(const char *zPath){
  int fd = open(zPath, O_RDONLY | O_CLOEXEC);
  struct stat st;
  long long nRes = -1;
  if( fd<0 ) return -1;
  if( fstat(fd, &st)==0 && st.st_size>0 ){
    long pg = sysconf(_SC_PAGESIZE);
    size_t nPg = (size_t)((st.st_size + pg - 1) / pg), i;
    void *pMap = mmap(0, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    unsigned char *aVec = malloc(nPg);
    if( pMap!=MAP_FAILED && aVec && mincore(pMap, (size_t)st.st_size, aVec)==0 ){
      nRes = 0;
      for(i=0; i<nPg; i++) nRes += (aVec[i] & 1);
    }
    free(aVec);
    if( pMap!=MAP_FAILED ) munmap(pMap, (size_t)st.st_size);
  }
  close(fd);
  return nRes;
}

/*----------------------------------------------------------------------------
** SQLite helpers
*/
static void execOrDie(sqlite3 *db, const char *zSql){
  char *zErr = 0;
  if( sqlite3_exec(db, zSql, 0, 0, &zErr)!=SQLITE_OK ){
    die("SQL failed: %s  [%s]", zErr ? zErr : sqlite3_errmsg(db), zSql);
  }
}
static void execFmt(sqlite3 *db, const char *zFmt, ...){
  char *zSql;
  va_list ap;
  va_start(ap, zFmt);
  zSql = sqlite3_vmprintf(zFmt, ap);
  va_end(ap);
  if( zSql==0 ) die("out of memory");
  execOrDie(db, zSql);
  sqlite3_free(zSql);
}
static sqlite3_stmt *prep(sqlite3 *db, const char *zSql){
  sqlite3_stmt *p = 0;
  if( sqlite3_prepare_v2(db, zSql, -1, &p, 0)!=SQLITE_OK ){
    die("prepare failed: %s  [%s]", sqlite3_errmsg(db), zSql);
  }
  return p;
}

/*----------------------------------------------------------------------------
** BF cache counters.  Reading PRAGMA bf_cache_stats gives (stat,value) rows on
** the BF build and zero rows on a stock build, so the same code path works for
** both and the JSON simply carries an empty object for stock.
*/
#define BF_MAX_STATS 96   /* was 40 while the pragma grew to 43 rows: the tail
                            ** (wal_commits, group_*) was silently dropped */
typedef struct BfStats {
  int n;
  char azName[BF_MAX_STATS][32];
  long long aVal[BF_MAX_STATS];
} BfStats;

static void bfStatsRead(sqlite3 *db, BfStats *p){
  sqlite3_stmt *q = 0;
  p->n = 0;
  if( sqlite3_prepare_v2(db, "PRAGMA bf_cache_stats;", -1, &q, 0)!=SQLITE_OK ){
    return;                      /* stock build: unknown pragma */
  }
  while( sqlite3_step(q)==SQLITE_ROW && p->n < BF_MAX_STATS ){
    const unsigned char *zName = sqlite3_column_text(q, 0);
    if( zName==0 ) continue;
    snprintf(p->azName[p->n], sizeof(p->azName[0]), "%s", (const char*)zName);
    p->aVal[p->n] = sqlite3_column_int64(q, 1);
    p->n++;
  }
  sqlite3_finalize(q);
}

/* Counters that are cumulative and therefore meaningful as a delta.  Config
** echoes like buffer_size must NOT be differenced. */
static int bfStatIsCumulative(const char *z){
  static const char *az[] = {
    "enabled", "buffer_size", "promotion_rate", "scan_promotion_rate",
    "read_record_cache", "adaptive_full_misses",
    "evict_target", "pool_retained_pages", "dirty_list_pages",
    "group_pending",
    /* Live gauges (current state, not a running total): differencing these
    ** across a run would report nonsense such as a negative cache size. */
    "cb_capacity", "map_batches", "map_entries", "live_mini_pages", "cached_records",
    "mini_page_bytes", 0
  };
  int i;
  for(i=0; az[i]; i++) if( strcmp(z, az[i])==0 ) return 0;
  return 1;
}

/*----------------------------------------------------------------------------
** Configuration
*/
#define OP_READ   0
#define OP_NREAD  1
#define OP_UPDATE 2
#define OP_INSERT 3
#define OP_SCAN   4
#define OP_RMW    5
#define OP_COMMIT 6
#define OP_COUNT  7

static const char *azOpName[OP_COUNT] = {
  "read", "negative_read", "update", "insert", "scan", "rmw", "commit"
};

typedef struct Config {
  const char *zDb;
  const char *zJson;
  const char *zLabel;
  const char *zSut;
  long long nRecords;
  int nValueLen;
  int nKeySpacing;
  int aMix[OP_COUNT];        /* percentages; OP_COMMIT unused in the mix */
  int dist;
  double theta;
  double seconds;
  long long nOps;
  double warmupSeconds;
  long long nWarmupOps;
  int nScanLen;
  long long bfCacheBytes;
  long long pageCacheBytes;
  const char *zSync;
  const char *zJournal;
  int nGroupCommit;
  int nPromotion;
  int nScanPromotion;      /* M3: PRAGMA bf_scan_promotion, -1 = build default */
  int nReadRecordCache;    /* B3b: PRAGMA bf_read_record_cache, -1 = default */
  int nAdaptiveFull;       /* X1: PRAGMA bf_adaptive_full, -1 = default */
  int nEvictTarget;        /* M4: PRAGMA bf_evict_target, -1 = build default */
  int nMinRecord;             /* PRAGMA bf_min_record; -1 leaves the default */
  int nCopyOnAccess;          /* PRAGMA bf_copy_on_access, percent; -1 = default */
  int bInsertReplace;         /* --insert-mode replace: INSERT OR REPLACE (upsert),
                              ** the reference's insert semantics and the shape the
                              ** D3b blind insert takes (plan D3b) */
  int bfEnable;              /* -1 = leave alone, 0/1 = PRAGMA bf_cache */
  int nOpsPerTxn;
  int nAutoCheckpoint;
  uint64_t seed;
  int bDropCache;
  int bNoLatency;
  int bReadTxn;
  int nMmap;
  int bDirectIo;              /* --direct-io: main db through the bfdio VFS (H7) */
} Config;

static void configDefaults(Config *p){
  memset(p, 0, sizeof(*p));
  p->zDb = "bench.db";
  p->zLabel = "run";
  p->zSut = "unknown";
  p->nRecords = 1000000;
  p->nValueLen = 100;
  p->nKeySpacing = 16;
  p->aMix[OP_READ] = 100;
  p->dist = DIST_ZIPF;
  p->nMinRecord = -1;
  p->nCopyOnAccess = -1;
  p->theta = 0.9;
  p->seconds = 0;
  p->nOps = 0;
  p->warmupSeconds = 0;
  p->nWarmupOps = 0;
  p->nScanLen = 32;
  p->bfCacheBytes = 0;
  p->pageCacheBytes = 0;
  p->zSync = "normal";
  p->zJournal = "wal";
  p->nGroupCommit = -1;
  p->nPromotion = -1;
  p->nScanPromotion = -1;
  p->nReadRecordCache = -1;
  p->nAdaptiveFull = -1;
  p->nEvictTarget = -1;
  p->bfEnable = -1;
  p->nOpsPerTxn = 1;
  p->nAutoCheckpoint = -1;
  p->seed = 7;
  p->nMmap = -1;
}

/* --workload read=50,update=50  (also accepts the full YCSB-ish set) */
static void parseMix(Config *p, const char *zSpec){
  char buf[512];
  char *z, *zTok, *zSave = 0;
  int i, total = 0;
  for(i=0; i<OP_COUNT; i++) p->aMix[i] = 0;
  snprintf(buf, sizeof(buf), "%s", zSpec);
  z = buf;
  for(zTok = strtok_r(z, ",", &zSave); zTok; zTok = strtok_r(0, ",", &zSave)){
    char *zEq = strchr(zTok, '=');
    int val, found = 0;
    if( zEq==0 ) die("bad --workload term '%s' (want name=pct)", zTok);
    *zEq = 0;
    val = atoi(zEq+1);
    for(i=0; i<OP_COUNT; i++){
      if( strcmp(zTok, azOpName[i])==0 ){ p->aMix[i] += val; found = 1; break; }
    }
    if( !found ) die("unknown workload op '%s'", zTok);
    if( i==OP_COMMIT ) die("'commit' is measured, not requested");
    total += val;
  }
  if( total != 100 ) die("--workload percentages sum to %d, want 100", total);
}

/*----------------------------------------------------------------------------
** Value generation.  Deterministic from the key, so a read can verify what it
** got back: byte j of the value for key k is a mix of k and j.  A run that is
** fast because it returned the wrong row shows up as read_errors, not as a win.
*/
static void makeValue(unsigned char *aBuf, int nBuf, long long key, int gen){
  uint64_t h = fnv64((uint64_t)key ^ ((uint64_t)gen << 40));
  int j;
  for(j=0; j<nBuf; j++){
    if( (j & 7)==0 ) h = fnv64(h + j);
    aBuf[j] = (unsigned char)((h >> ((j & 7) * 8)) & 0xff);
  }
}
static int checkValue(const unsigned char *a, int n, long long key, int nWant){
  unsigned char exp[8];
  if( n != nWant ) return 0;
  makeValue(exp, 8, key, 0);
  /* Generation 0 is what load wrote; an updated row has gen>0 and legitimately
  ** differs, so only the length plus the first-8-bytes-of-some-generation test
  ** is universal.  We therefore accept any row whose length matches and whose
  ** bytes match SOME generation we could have written (0 or the update marker
  ** stored in the last byte). */
  if( memcmp(a, exp, 8)==0 ) return 1;
  {
    int gen = a[n-1];
    makeValue(exp, 8, key, gen);
    return memcmp(a, exp, 8)==0;
  }
}

/*----------------------------------------------------------------------------
** load
*/
static int cmdLoad(Config *p){
  sqlite3 *db = 0;
  sqlite3_stmt *pIns;
  unsigned char *aVal;
  long long i;
  double t0;

  unlink(p->zDb);
  { char buf[4096];
    snprintf(buf, sizeof(buf), "%s-wal", p->zDb); unlink(buf);
    snprintf(buf, sizeof(buf), "%s-shm", p->zDb); unlink(buf); }

  if( sqlite3_open(p->zDb, &db)!=SQLITE_OK ) die("open: %s", sqlite3_errmsg(db));
  execOrDie(db, "PRAGMA journal_mode=off;");
  execOrDie(db, "PRAGMA synchronous=off;");
  execOrDie(db, "PRAGMA cache_size=-262144;");        /* 256 MB while loading */
  execFmt(db, "PRAGMA page_size=4096;");
  execOrDie(db, "CREATE TABLE usertable(id INTEGER PRIMARY KEY, v BLOB);");

  aVal = malloc(p->nValueLen);
  if( aVal==0 ) die("out of memory");
  pIns = prep(db, "INSERT INTO usertable VALUES(?,?)");
  t0 = nowSec();
  execOrDie(db, "BEGIN");
  for(i=1; i<=p->nRecords; i++){
    long long key = i * p->nKeySpacing;
    makeValue(aVal, p->nValueLen, key, 0);
    sqlite3_bind_int64(pIns, 1, key);
    sqlite3_bind_blob(pIns, 2, aVal, p->nValueLen, SQLITE_STATIC);
    if( sqlite3_step(pIns)!=SQLITE_DONE ) die("insert: %s", sqlite3_errmsg(db));
    sqlite3_reset(pIns);
    if( (i % 200000)==0 ){
      execOrDie(db, "COMMIT");
      execOrDie(db, "BEGIN");
      if( (i % 5000000)==0 ){
        fprintf(stderr, "  loaded %lld / %lld rows (%.0fs)\n",
                i, p->nRecords, nowSec()-t0);
      }
    }
  }
  execOrDie(db, "COMMIT");
  sqlite3_finalize(pIns);
  free(aVal);

  /* Leave the file in a canonical, checkpointed state so every SUT starts from
  ** a byte-identical base tree. */
  execOrDie(db, "PRAGMA journal_mode=delete;");
  sqlite3_close(db);

  fprintf(stderr, "loaded %lld rows into %s (%.2f GiB) in %.1fs\n",
          p->nRecords, p->zDb, fileSize(p->zDb)/(double)(1<<30), nowSec()-t0);
  return 0;
}

/*----------------------------------------------------------------------------
** run
*/
typedef struct RunState {
  sqlite3 *db;
  sqlite3_stmt *pRead;
  sqlite3_stmt *pUpdate;
  sqlite3_stmt *pInsert;
  sqlite3_stmt *pScan;
  Rng rng;
  Zipf zipf;
  Feistel gaps;
  long long nInsertSeq;
  long long nGapSlots;
  unsigned char *aVal;
  Hist aHist[OP_COUNT];
  long long aCount[OP_COUNT];
  long long nReadErr;
  long long nReadMiss;
  long long nConstraint;
  long long nScanRows;
  int updateGen;
} RunState;

/* Pick an existing key according to the configured distribution. */
static long long pickKey(RunState *s, Config *p){
  uint64_t idx;
  switch( p->dist ){
    case DIST_UNIFORM:
      idx = rngNext(&s->rng) % (uint64_t)p->nRecords;
      break;
    case DIST_LATEST: {
      /* YCSB "latest": Zipf over recency, i.e. rank 0 is the newest key. */
      uint64_t r = zipfNext(&s->zipf, &s->rng);
      idx = (uint64_t)p->nRecords - 1 - (r % (uint64_t)p->nRecords);
      break;
    }
    case DIST_ZIPFRAW:
      /* Raw Zipfian: the rank IS the key index, so the hot set is the first N
      ** physically adjacent rows.  This is what the Bf-Tree reference harness
      ** does -- benchmark/src/common.rs samples rand_distr::Zipf and
      ** install_value_to_buffer() writes that rank straight into the key --
      ** and therefore what the paper's numbers were measured on.
      **
      ** It is the FRIENDLIER setting for a page cache, not a hostile one: a
      ** 4 KiB leaf holding ~34 adjacent hot rows is a good deal for stock.
      ** What it changes for us is mini-page occupancy.  Under the scrambled
      ** default our mini-pages hold 1.26 records each (live_mini_pages 414574
      ** vs cached_records 522780 in the 2026-09-16 campaign), so we pay a
      ** header + meta + size-class round-up per SINGLE record -- 250 B to
      ** cache a 116 B one.  Clustered keys amortise that header across many
      ** records, and they are the precondition for the full-page cache
      ** (BF_LOC_FULL) being an admission win rather than ring waste.
      **
      ** Both modes stay, and both get reported.  Neither is "the honest one":
      ** YCSB scrambles, so the default is the conservative choice, while this
      ** is the one that reproduces the paper.  Quoting only whichever flatters
      ** the result is the thing to avoid. */
      idx = zipfNext(&s->zipf, &s->rng) % (uint64_t)p->nRecords;
      break;
    default:
      /* Scrambled Zipfian: rank -> hash -> key, so the hot set is spread over
      ** the whole file rather than the first few pages. */
      idx = fnv64(zipfNext(&s->zipf, &s->rng)) % (uint64_t)p->nRecords;
      break;
  }
  return ((long long)idx + 1) * p->nKeySpacing;
}

/* A key that is guaranteed absent: a gap slot far past the insert frontier. */
static long long pickAbsentKey(RunState *s, Config *p){
  uint64_t slot;
  long long base, off;
  if( s->nGapSlots<=0 ) return -1;
  /* Draw from the tail half of the permutation, which the insert stream has
  ** not reached (it consumes slots from index 0 upward). */
  slot = feistelAt(&s->gaps,
                   (uint64_t)s->nGapSlots/2 + rngNext(&s->rng) % (uint64_t)(s->nGapSlots/2 + 1));
  base = (long long)(slot / (uint64_t)(p->nKeySpacing - 1)) + 1;
  off  = (long long)(slot % (uint64_t)(p->nKeySpacing - 1)) + 1;
  return base * p->nKeySpacing + off;
}

/* The next never-before-used insert key. */
static long long nextInsertKey(RunState *s, Config *p){
  uint64_t slot;
  long long base, off;
  if( s->nGapSlots<=0 ) return -1;
  /* Only the first half of the slot space is handed to inserts; the second
  ** half is reserved for negative reads, so the two never collide. */
  slot = feistelAt(&s->gaps, (uint64_t)(s->nInsertSeq++ % (s->nGapSlots/2 + 1)));
  base = (long long)(slot / (uint64_t)(p->nKeySpacing - 1)) + 1;
  off  = (long long)(slot % (uint64_t)(p->nKeySpacing - 1)) + 1;
  return base * p->nKeySpacing + off;
}

static void doRead(RunState *s, Config *p, long long key, int bNegative){
  int rc;
  sqlite3_bind_int64(s->pRead, 1, key);
  rc = sqlite3_step(s->pRead);
  if( rc==SQLITE_ROW ){
    if( bNegative ){
      s->nReadErr++;                       /* should not have existed */
    }else{
      const unsigned char *a = sqlite3_column_blob(s->pRead, 0);
      int n = sqlite3_column_bytes(s->pRead, 0);
      if( a==0 || !checkValue(a, n, key, p->nValueLen) ) s->nReadErr++;
    }
  }else if( rc==SQLITE_DONE ){
    if( !bNegative ) s->nReadMiss++;       /* row should have existed */
  }else{
    die("read: %s", sqlite3_errmsg(s->db));
  }
  sqlite3_reset(s->pRead);
}

static void doUpdate(RunState *s, Config *p, long long key){
  int gen = 1 + (s->updateGen++ & 0x7e);
  makeValue(s->aVal, p->nValueLen, key, gen);
  s->aVal[p->nValueLen-1] = (unsigned char)gen;   /* so checkValue can verify */
  sqlite3_bind_blob(s->pUpdate, 1, s->aVal, p->nValueLen, SQLITE_STATIC);
  sqlite3_bind_int64(s->pUpdate, 2, key);
  if( sqlite3_step(s->pUpdate)!=SQLITE_DONE ) die("update: %s",
                                                  sqlite3_errmsg(s->db));
  sqlite3_reset(s->pUpdate);
}

static void doInsert(RunState *s, Config *p){
  long long key = nextInsertKey(s, p);
  int rc;
  if( key < 0 ) return;
  makeValue(s->aVal, p->nValueLen, key, 0);
  sqlite3_bind_int64(s->pInsert, 1, key);
  sqlite3_bind_blob(s->pInsert, 2, s->aVal, p->nValueLen, SQLITE_STATIC);
  rc = sqlite3_step(s->pInsert);
  if( rc!=SQLITE_DONE ){
    if( sqlite3_extended_errcode(s->db)==SQLITE_CONSTRAINT_PRIMARYKEY ){
      s->nConstraint++;                    /* permutation wrapped; not fatal */
    }else{
      die("insert: %s", sqlite3_errmsg(s->db));
    }
  }
  sqlite3_reset(s->pInsert);
}

static void doScan(RunState *s, Config *p, long long key){
  int rc;
  sqlite3_bind_int64(s->pScan, 1, key);   /* LIMIT is bound once, at prepare */
  while( (rc = sqlite3_step(s->pScan))==SQLITE_ROW ){
    /* Touch the payload so the scan cannot be optimised into an index-only
    ** walk that never materialises a row. */
    const unsigned char *a = sqlite3_column_blob(s->pScan, 1);
    int n = sqlite3_column_bytes(s->pScan, 1);
    if( a && n>0 ) s->nScanRows += (a[0] & 1) ? 1 : 1;
  }
  if( rc!=SQLITE_DONE ) die("scan: %s", sqlite3_errmsg(s->db));
  sqlite3_reset(s->pScan);
}

/* Execute one operation of the given type.  Returns nothing; the caller times
** it.  RMW is deliberately NOT wrapped in its own transaction here -- the
** --ops-per-txn machinery owns transaction boundaries so that the commit cost
** is attributed to the "commit" op type and not smeared across the mix. */
static void doOp(RunState *s, Config *p, int op){
  switch( op ){
    case OP_READ:   doRead(s, p, pickKey(s, p), 0); break;
    case OP_NREAD:  doRead(s, p, pickAbsentKey(s, p), 1); break;
    case OP_UPDATE: doUpdate(s, p, pickKey(s, p)); break;
    case OP_INSERT: doInsert(s, p); break;
    case OP_SCAN:   doScan(s, p, pickKey(s, p)); break;
    case OP_RMW: {
      long long key = pickKey(s, p);
      doRead(s, p, key, 0);
      doUpdate(s, p, key);
      break;
    }
  }
}

static int pickOp(RunState *s, Config *p){
  int r = (int)(rngNext(&s->rng) % 100);
  int acc = 0, i;
  for(i=0; i<OP_COUNT; i++){
    acc += p->aMix[i];
    if( r < acc ) return i;
  }
  return OP_READ;
}

static int mixHasWrites(Config *p){
  return p->aMix[OP_UPDATE] || p->aMix[OP_INSERT] || p->aMix[OP_RMW];
}

/*----------------------------------------------------------------------------
** JSON output
*/
static void jsonHist(FILE *f, const char *zName, Hist *h, long long nCount){
  fprintf(f, "      \"%s\": {\"count\": %lld, \"mean_ns\": %.1f, "
             "\"p50_ns\": %llu, \"p90_ns\": %llu, \"p99_ns\": %llu, "
             "\"p999_ns\": %llu, \"p9999_ns\": %llu, \"max_ns\": %llu}",
          zName, nCount,
          h->count ? (double)h->total / (double)h->count : 0.0,
          (unsigned long long)histPct(h, 50.0),
          (unsigned long long)histPct(h, 90.0),
          (unsigned long long)histPct(h, 99.0),
          (unsigned long long)histPct(h, 99.9),
          (unsigned long long)histPct(h, 99.99),
          (unsigned long long)h->max);
}

/* Confirm the open database is the one this run was configured for.  Cheap:
** two B-tree lookups at the ends of the key space plus a value check.  Dies
** (exit 1, not the read-error exit 3) so the runner aborts immediately rather
** than recording 216 fast-and-wrong results.
*/
static void verifyDataset(RunState *s, Config *p){
  sqlite3_stmt *pQ;
  long long loKey = p->nKeySpacing;
  long long hiKey = p->nRecords * (long long)p->nKeySpacing;
  int i;
  pQ = prep(s->db, "SELECT v FROM usertable WHERE id=?");
  for(i=0; i<2; i++){
    long long key = i ? hiKey : loKey;
    const unsigned char *a;
    int n;
    sqlite3_bind_int64(pQ, 1, key);
    if( sqlite3_step(pQ)!=SQLITE_ROW ){
      die("dataset mismatch: key %lld (%s of a %lld-row, spacing-%d table) is "
          "absent from %s.  The database is not the one this run expects.",
          key, i ? "last" : "first", p->nRecords, p->nKeySpacing, p->zDb);
    }
    a = sqlite3_column_blob(pQ, 0);
    n = sqlite3_column_bytes(pQ, 0);
    if( a==0 || n!=p->nValueLen ){
      die("dataset mismatch: key %lld holds a %d-byte value, --value-len says "
          "%d (%s)", key, n, p->nValueLen, p->zDb);
    }
    if( !checkValue(a, n, key, p->nValueLen) ){
      die("dataset mismatch: value at key %lld does not match the generator "
          "(%s)", key, p->zDb);
    }
    sqlite3_reset(pQ);
  }
  /* Nothing beyond the declared key space: a bigger table means a different
  ** dataset, which would silently change the miss rate. */
  sqlite3_finalize(pQ);
  pQ = prep(s->db, "SELECT max(id) FROM usertable");
  if( sqlite3_step(pQ)==SQLITE_ROW && sqlite3_column_int64(pQ, 0)!=hiKey ){
    die("dataset mismatch: max(id)=%lld, expected %lld (%lld rows x spacing %d)"
        " in %s", (long long)sqlite3_column_int64(pQ, 0), hiKey,
        p->nRecords, p->nKeySpacing, p->zDb);
  }
  sqlite3_finalize(pQ);
}

static int cmdRun(Config *p){
  sqlite3 *db = 0;
  RunState s;
  ProcIo io0, io1;
  struct rusage ru0, ru1;
  Pmu pmu;
  BfStats bf0, bf1;
  long long dbSize0, walSize0, dbSize1, walSize1, walMax = 0;
  int dbCacheHit0=0, dbCacheMiss0=0, dbCacheWrite0=0, dbCacheSpill0=0, dummy;
  int dbCacheHit1=0, dbCacheMiss1=0, dbCacheWrite1=0, dbCacheSpill1=0;
  int memUsed, memHigh;
  double tStart, tEnd, elapsed;
  long long nDone = 0, nTxn = 0;
  long long nRes0, nRes1, nWalRes0, nWalRes1;
  int inTxn = 0, i;
  FILE *out;

  memset(&s, 0, sizeof(s));

  if( p->bDropCache ) dropFileCache(p->zDb);

  if( p->bDirectIo ){
    if( p->nMmap > 0 ) die("--direct-io with --mmap: mmap bypasses the VFS read path");
    dioRegister();
  }
  if( sqlite3_open_v2(p->zDb, &db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,
                      p->bDirectIo ? "bfdio" : 0)!=SQLITE_OK ){
    die("open: %s", sqlite3_errmsg(db));
  }
  s.db = db;

  /* Must come before anything creates a pager: PRAGMA bf_cache swaps the global
  ** pcache methods, which only apply to page caches created after the change. */
  if( p->bfEnable >= 0 ) execFmt(db, "PRAGMA bf_cache=%d;", p->bfEnable);

  /* ---- engine configuration -------------------------------------------
  ** Memory budget: the BF build spends it on records (bf_cache_size) and keeps
  ** a deliberately small page cache; stock spends the identical number of bytes
  ** on the page cache.  That is the "same bytes, records vs pages" comparison.
  */
  if( p->pageCacheBytes > 0 ){
    execFmt(db, "PRAGMA cache_size=-%lld;", p->pageCacheBytes / 1024);
  }
  execFmt(db, "PRAGMA journal_mode=%s;", p->zJournal);
  execFmt(db, "PRAGMA synchronous=%s;", p->zSync);
  if( p->nAutoCheckpoint >= 0 ){
    execFmt(db, "PRAGMA wal_autocheckpoint=%d;", p->nAutoCheckpoint);
  }
  execFmt(db, "PRAGMA mmap_size=%d;", p->nMmap >= 0 ? p->nMmap : 0);

  /* BF settings go last: PRAGMA journal_mode reopens the pager, and anything
  ** configured on the BF cache before that point does not survive it. */
  if( p->bfCacheBytes > 0 ){
    execFmt(db, "PRAGMA bf_cache_size=%lld;", p->bfCacheBytes);
  }
  /* AFTER bf_cache_size: setting bf_min_record rebuilds the size-class ladder
  ** by dropping every mapping and reinitialising the ring at its current
  ** capacity, so it has to see the capacity this run actually wants. */
  if( p->nMinRecord > 0 )    execFmt(db, "PRAGMA bf_min_record=%d;", p->nMinRecord);
  if( p->nPromotion >= 0 )   execFmt(db, "PRAGMA bf_promotion_rate=%d;", p->nPromotion);
  if( p->nScanPromotion >= 0 ){
    execFmt(db, "PRAGMA bf_scan_promotion=%d;", p->nScanPromotion);
  }
  if( p->nReadRecordCache >= 0 ){
    execFmt(db, "PRAGMA bf_read_record_cache=%d;", p->nReadRecordCache);
  }
  if( p->nAdaptiveFull >= 0 ){
    execFmt(db, "PRAGMA bf_adaptive_full=%d;", p->nAdaptiveFull);
  }
  if( p->nEvictTarget >= 0 ){
    execFmt(db, "PRAGMA bf_evict_target=%d;", p->nEvictTarget);
  }
  /* A global the ring re-reads on every (re)initialisation, so its order
  ** relative to bf_cache_size / bf_min_record does not matter. */
  if( p->nCopyOnAccess >= 0 ) execFmt(db, "PRAGMA bf_copy_on_access=%d;", p->nCopyOnAccess);
  if( p->nGroupCommit >= 0 ) execFmt(db, "PRAGMA bf_group_commit=%d;", p->nGroupCommit);
  execOrDie(db, "PRAGMA temp_store=MEMORY;");

  s.pRead   = prep(db, "SELECT v FROM usertable WHERE id=?");
  s.pUpdate = prep(db, "UPDATE usertable SET v=? WHERE id=?");
  s.pInsert = prep(db, p->bInsertReplace
                      ? "INSERT OR REPLACE INTO usertable VALUES(?,?)"
                      : "INSERT INTO usertable VALUES(?,?)");
  s.pScan   = prep(db, "SELECT id,v FROM usertable WHERE id>=? ORDER BY id LIMIT ?");
  /* Bind LIMIT ONCE.  SQLite reads a bound LIMIT at prepare time
  ** (sqlite3ExprIsInteger -> sqlite3VdbeSetVarmask, expr.c), and re-binding a
  ** masked parameter -- even to the same value -- expires the statement
  ** (vdbeapi.c vdbeUnbind), so binding it per scan recompiled the SQL on every
  ** scan: 43.6% of stock's scan CPU was sqlite3Reprepare (perf, 2026-10-05).
  ** Every scan campaign before this fix paid it, all SUTs alike. */
  sqlite3_bind_int(s.pScan, 2, p->nScanLen);

  s.aVal = malloc(p->nValueLen);
  if( s.aVal==0 ) die("out of memory");

  /* ---- dataset sanity --------------------------------------------------
  ** The single most expensive harness bug is running a workload against the
  ** WRONG database: every read misses, the run looks blazing fast, and the
  ** only symptom is a read_misses count nobody reads until the campaign is
  ** over.  Check the template matches --records/--value-len/--key-spacing
  ** BEFORE the zipf table and the warmup burn 25 seconds per run.  This is
  ** two index lookups.
  */
  verifyDataset(&s, p);

  rngSeed(&s.rng, p->seed);
  if( p->dist!=DIST_UNIFORM ){
    double t = nowSec();
    zipfInit(&s.zipf, (uint64_t)p->nRecords, p->theta);
    fprintf(stderr, "  zipf(theta=%.2f, n=%lld) precomputed in %.1fs\n",
            p->theta, p->nRecords, nowSec()-t);
  }
  s.nGapSlots = p->nRecords * (p->nKeySpacing - 1);
  feistelInit(&s.gaps, (uint64_t)s.nGapSlots, p->seed ^ 0x5eed);

  /* ---- warmup ---------------------------------------------------------- */
  if( p->warmupSeconds > 0 || p->nWarmupOps > 0 ){
    double tw = nowSec();
    long long nw = 0;
    while( (p->nWarmupOps  && nw < p->nWarmupOps)
        || (p->warmupSeconds && nowSec()-tw < p->warmupSeconds) ){
      int op = pickOp(&s, p);
      if( p->nOpsPerTxn > 1 && !inTxn ){ execOrDie(db, "BEGIN"); inTxn = 1; }
      doOp(&s, p, op);
      nw++;
      if( inTxn && (nw % p->nOpsPerTxn)==0 ){ execOrDie(db,"COMMIT"); inTxn = 0; }
    }
    if( inTxn ){ execOrDie(db, "COMMIT"); inTxn = 0; }
    fprintf(stderr, "  warmup: %lld ops in %.1fs\n", nw, nowSec()-tw);
    /* Reset per-op stats gathered during warmup. */
    memset(s.aHist, 0, sizeof(s.aHist));
    memset(s.aCount, 0, sizeof(s.aCount));
    s.nReadErr = s.nReadMiss = s.nConstraint = s.nScanRows = 0;
  }

  /* ---- snapshot counters at the start of the MEASURED phase ------------- */
  { char buf[4096];
    dbSize0 = fileSize(p->zDb);
    snprintf(buf, sizeof(buf), "%s-wal", p->zDb);
    walSize0 = fileSize(buf); }
  bfStatsRead(db, &bf0);
  sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_HIT,   &dbCacheHit0,  &dummy, 1);
  sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_MISS,  &dbCacheMiss0, &dummy, 1);
  sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_WRITE, &dbCacheWrite0,&dummy, 1);
  sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_SPILL, &dbCacheSpill0,&dummy, 1);
  nRes0 = residentPages(p->zDb);
  { char b[512]; snprintf(b, sizeof(b), "%s-wal", p->zDb); nWalRes0 = residentPages(b); }
  memset(&gDio, 0, sizeof(gDio));     /* direct-I/O counters: measured phase only */
  procIoRead(&io0);
  getrusage(RUSAGE_SELF, &ru0);
  pmuOpen(&pmu);

  /* ---- measured phase --------------------------------------------------- */
  if( p->bReadTxn && !mixHasWrites(p) ){
    execOrDie(db, "BEGIN");                /* one long read transaction */
    inTxn = 1;
  }
  pmuStart(&pmu);
  tStart = nowSec();
  for(;;){
    int op;
    uint64_t t0, t1;
    if( p->nOps  && nDone >= p->nOps ) break;
    if( p->seconds && (nDone & 0x3ff)==0 && nowSec()-tStart >= p->seconds ) break;
    if( !p->nOps && !p->seconds ) break;

    if( p->nOpsPerTxn > 1 && !inTxn ){ execOrDie(db, "BEGIN"); inTxn = 1; }

    op = pickOp(&s, p);
    if( p->bNoLatency ){
      doOp(&s, p, op);
    }else{
      t0 = nowNs();
      doOp(&s, p, op);
      t1 = nowNs();
      histAdd(&s.aHist[op], t1 - t0);
    }
    s.aCount[op]++;
    nDone++;

    if( inTxn && !(p->bReadTxn && !mixHasWrites(p))
              && (nDone % p->nOpsPerTxn)==0 ){
      uint64_t c0 = nowNs();
      execOrDie(db, "COMMIT");
      if( !p->bNoLatency ) histAdd(&s.aHist[OP_COMMIT], nowNs() - c0);
      s.aCount[OP_COMMIT]++;
      nTxn++;
      inTxn = 0;
      if( (nTxn & 0xff)==0 ){
        char buf[4096];
        long long w;
        snprintf(buf, sizeof(buf), "%s-wal", p->zDb);
        w = fileSize(buf);
        if( w > walMax ) walMax = w;
      }
    }
  }
  if( inTxn ){
    uint64_t c0 = nowNs();
    execOrDie(db, "COMMIT");
    if( !p->bNoLatency ) histAdd(&s.aHist[OP_COMMIT], nowNs() - c0);
    s.aCount[OP_COMMIT]++;
    nTxn++;
    inTxn = 0;
  }
  tEnd = nowSec();
  pmuStop(&pmu);
  elapsed = tEnd - tStart;

  /* ---- snapshot counters at the end ------------------------------------ */
  getrusage(RUSAGE_SELF, &ru1);
  procIoRead(&io1);
  nRes1 = residentPages(p->zDb);
  { char b[512]; snprintf(b, sizeof(b), "%s-wal", p->zDb); nWalRes1 = residentPages(b); }
  sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_HIT,   &dbCacheHit1,  &dummy, 0);
  sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_MISS,  &dbCacheMiss1, &dummy, 0);
  sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_WRITE, &dbCacheWrite1,&dummy, 0);
  sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_SPILL, &dbCacheSpill1,&dummy, 0);
  bfStatsRead(db, &bf1);
  { char buf[4096];
    dbSize1 = fileSize(p->zDb);
    snprintf(buf, sizeof(buf), "%s-wal", p->zDb);
    walSize1 = fileSize(buf);
    if( walSize1 > walMax ) walMax = walSize1; }
  memUsed = (int)sqlite3_memory_used();
  memHigh = (int)sqlite3_memory_highwater(0);

  sqlite3_finalize(s.pRead);
  sqlite3_finalize(s.pUpdate);
  sqlite3_finalize(s.pInsert);
  sqlite3_finalize(s.pScan);

  /* ---- report ---------------------------------------------------------- */
  out = p->zJson ? fopen(p->zJson, "w") : stdout;
  if( out==0 ) die("cannot write %s", p->zJson);

  fprintf(out, "{\n");
  fprintf(out, "  \"label\": \"%s\",\n", p->zLabel);
  fprintf(out, "  \"sut\": \"%s\",\n", p->zSut);
  fprintf(out, "  \"config\": {\n");
  fprintf(out, "    \"db\": \"%s\", \"records\": %lld, \"value_len\": %d, "
               "\"key_spacing\": %d,\n",
          p->zDb, p->nRecords, p->nValueLen, p->nKeySpacing);
  fprintf(out, "    \"dist\": \"%s\", \"theta\": %.3f, \"scan_len\": %d,\n",
          p->dist==DIST_UNIFORM ? "uniform" :
          p->dist==DIST_LATEST  ? "latest" :
          p->dist==DIST_ZIPFRAW ? "zipf-raw" : "zipf",
          p->theta, p->nScanLen);
  fprintf(out, "    \"mix\": {");
  for(i=0; i<OP_COMMIT; i++){
    fprintf(out, "%s\"%s\": %d", i?", ":"", azOpName[i], p->aMix[i]);
  }
  fprintf(out, "},\n");
  fprintf(out, "    \"bf_cache_bytes\": %lld, \"page_cache_bytes\": %lld,\n",
          p->bfCacheBytes, p->pageCacheBytes);
  fprintf(out, "    \"synchronous\": \"%s\", \"journal_mode\": \"%s\", "
               "\"group_commit\": %d, \"promotion_rate\": %d, "
               "\"scan_promotion\": %d, \"read_record_cache\": %d, "
               "\"adaptive_full\": %d, "
               "\"evict_target\": %d, "
               "\"copy_on_access\": %d, \"min_record\": %d, "
               "\"insert_mode\": \"%s\",\n",
          p->zSync, p->zJournal, p->nGroupCommit, p->nPromotion,
          p->nScanPromotion, p->nReadRecordCache, p->nAdaptiveFull,
          p->nEvictTarget,
          p->nCopyOnAccess, p->nMinRecord,
          p->bInsertReplace ? "replace" : "plain");
  fprintf(out, "    \"ops_per_txn\": %d, \"seed\": %llu, "
               "\"read_txn\": %d, \"drop_cache\": %d,\n",
          p->nOpsPerTxn, (unsigned long long)p->seed, p->bReadTxn,
          p->bDropCache);
  fprintf(out, "    \"direct_io\": %d\n", p->bDirectIo);
  fprintf(out, "  },\n");

  fprintf(out, "  \"result\": {\n");
  fprintf(out, "    \"elapsed_s\": %.6f,\n", elapsed);
  fprintf(out, "    \"ops\": %lld,\n", nDone);
  fprintf(out, "    \"throughput_ops_s\": %.1f,\n",
          elapsed>0 ? nDone/elapsed : 0.0);
  fprintf(out, "    \"txns\": %lld,\n", nTxn);
  fprintf(out, "    \"txn_throughput_s\": %.1f,\n",
          elapsed>0 ? nTxn/elapsed : 0.0);
  fprintf(out, "    \"read_errors\": %lld,\n", s.nReadErr);
  fprintf(out, "    \"read_misses\": %lld,\n", s.nReadMiss);
  fprintf(out, "    \"insert_constraint_hits\": %lld,\n", s.nConstraint);
  fprintf(out, "    \"scan_rows\": %lld,\n", s.nScanRows);

  fprintf(out, "    \"latency\": {\n");
  { int first = 1;
    for(i=0; i<OP_COUNT; i++){
      if( s.aCount[i]==0 ) continue;
      if( !first ) fprintf(out, ",\n");
      first = 0;
      jsonHist(out, azOpName[i], &s.aHist[i], s.aCount[i]);
    }
    fprintf(out, "\n    },\n"); }

  fprintf(out, "    \"cpu\": {\"user_s\": %.3f, \"sys_s\": %.3f, "
               "\"maxrss_kb\": %ld, \"minflt\": %ld, \"majflt\": %ld},\n",
          (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec)
            + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec)*1e-6,
          (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec)
            + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec)*1e-6,
          ru1.ru_maxrss,
          ru1.ru_minflt - ru0.ru_minflt,
          ru1.ru_majflt - ru0.ru_majflt);

  pmuJson(out, &pmu, nDone);

  fprintf(out, "    \"io\": {\"read_bytes\": %llu, \"write_bytes\": %llu, "
               "\"rchar\": %llu, \"wchar\": %llu, \"syscr\": %llu, "
               "\"syscw\": %llu, \"cancelled_write_bytes\": %llu},\n",
          (unsigned long long)(io1.read_bytes - io0.read_bytes),
          (unsigned long long)(io1.write_bytes - io0.write_bytes),
          (unsigned long long)(io1.rchar - io0.rchar),
          (unsigned long long)(io1.wchar - io0.wchar),
          (unsigned long long)(io1.syscr - io0.syscr),
          (unsigned long long)(io1.syscw - io0.syscw),
          (unsigned long long)(io1.cancelled - io0.cancelled));

  fprintf(out, "    \"files\": {\"db_bytes_before\": %lld, "
               "\"db_bytes_after\": %lld, \"wal_bytes_before\": %lld, "
               "\"wal_bytes_after\": %lld, \"wal_bytes_max\": %lld},\n",
          dbSize0, dbSize1, walSize0, walSize1, walMax);

  /* Direct-I/O proof (H7): with --direct-io the db's OS-cached pages must not
  ** grow over the measured phase; reported for buffered runs too, where they
  ** show how much of the file the OS cache is quietly serving. */
  fprintf(out, "    \"dio\": {\"direct_io\": %d, \"db_resident_pages_before\": %lld, "
               "\"db_resident_pages_after\": %lld, \"reads\": %llu, "
               "\"read_bytes\": %llu, \"reads_unaligned\": %llu, "
               "\"short_reads\": %llu, \"writes\": %llu, \"write_bytes\": %llu, "
               "\"write_fallbacks\": %llu, \"wal_direct\": %d, "
               "\"wal_resident_pages_before\": %lld, \"wal_resident_pages_after\": %lld, "
               "\"wal_reads\": %llu, \"wal_read_bytes\": %llu, \"wal_rmw_reads\": %llu, "
               "\"wal_short_reads\": %llu, \"wal_writes\": %llu, \"wal_write_bytes\": %llu, "
               "\"wal_logical_bytes\": %llu, \"wal_tail_flushes\": %llu},\n",
          p->bDirectIo, nRes0, nRes1, gDio.nRead, gDio.nReadBytes,
          gDio.nReadUnaligned, gDio.nShort, gDio.nWrite, gDio.nWriteBytes,
          gDio.nWriteFallback, p->bDirectIo, nWalRes0, nWalRes1,
          gDio.nWalRead, gDio.nWalReadBytes, gDio.nWalRmw, gDio.nWalShort,
          gDio.nWalWrite, gDio.nWalWriteBytes, gDio.nWalLogicalBytes,
          gDio.nWalTailFlush);

  fprintf(out, "    \"pcache\": {\"hit\": %d, \"miss\": %d, \"write\": %d, "
               "\"spill\": %d},\n",
          dbCacheHit1, dbCacheMiss1, dbCacheWrite1, dbCacheSpill1);
  fprintf(out, "    \"sqlite_memory\": {\"used\": %d, \"highwater\": %d},\n",
          memUsed, memHigh);

  /* BF counters as deltas over the measured phase (config echoes as-is). */
  fprintf(out, "    \"bf\": {");
  { int first = 1, j, k;
    for(j=0; j<bf1.n; j++){
      long long v = bf1.aVal[j];
      if( bfStatIsCumulative(bf1.azName[j]) ){
        for(k=0; k<bf0.n; k++){
          if( strcmp(bf0.azName[k], bf1.azName[j])==0 ){ v -= bf0.aVal[k]; break; }
        }
      }
      fprintf(out, "%s\"%s\": %lld", first?"":", ", bf1.azName[j], v);
      first = 0;
    }
    fprintf(out, "}\n"); }

  fprintf(out, "  }\n}\n");
  if( out!=stdout ) fclose(out);

  /* Quote the latency of the mix's DOMINANT op, not always the read: on a
  ** write workload the read histogram is empty and printing it as "p50=0.0us"
  ** makes a live run look like it is doing nothing. */
  { int iDom = OP_READ; long long best = -1;
    for(i=0; i<OP_COMMIT; i++){
      if( s.aCount[i] > best ){ best = s.aCount[i]; iDom = i; }
    }
  fprintf(stderr,
      "%-10s %-22s %8.0f ops/s  %s p50=%.1fus p99=%.1fus  "
      "read_MiB=%.1f write_MiB=%.1f%s\n",
      p->zSut, p->zLabel, elapsed>0 ? nDone/elapsed : 0.0,
      azOpName[iDom],
      histPct(&s.aHist[iDom], 50.0)/1000.0,
      histPct(&s.aHist[iDom], 99.0)/1000.0,
      (io1.read_bytes - io0.read_bytes)/(double)(1<<20),
      (io1.write_bytes - io0.write_bytes)/(double)(1<<20),
      (s.nReadErr || s.nReadMiss) ? "  *** READ ERRORS ***" : ""); }

  free(s.aVal);
  sqlite3_close(db);
  return (s.nReadErr || s.nReadMiss) ? 3 : 0;
}

/*----------------------------------------------------------------------------
** Argument parsing
*/
static const char zUsage[] =
"usage:\n"
"  bfbench load --db PATH --records N [--value-len 100] [--key-spacing 16]\n"
"  bfbench run  --db PATH --records N [options]\n"
"\n"
"run options:\n"
"  --workload read=100,update=0,insert=0,scan=0,negative_read=0,rmw=0\n"
"  --dist zipf|zipf-raw|uniform|latest   --theta 0.9      --scan-len 32\n"
"       zipf     = YCSB scrambled rank (hot set spread over the key space)\n"
"       zipf-raw = rank used directly as the key, as the Bf-Tree reference\n"
"                  harness does (hot set contiguous); reproduces the paper\n"
"  --seconds S | --ops N        --warmup-seconds S | --warmup-ops N\n"
"  --bf-cache-bytes N           --page-cache-bytes N\n"
"  --synchronous off|normal|full  --journal wal|delete\n"
"  --group-commit N             --promotion N    --bf-cache on|off\n"
"  --scan-promotion N           per-leaf scan full-page rate (M3)\n"
"  --read-record-cache 0|1      B3b: read miss caches the record (1) or leaf (0)\n"
"  --adaptive-full N            X1 experiment: leaf goes full after N mini misses\n"
"  --evict-target N             bytes per eviction batch, 0 = legacy (M4)\n"
"  --copy-on-access N           PRAGMA bf_copy_on_access, percent of the ring\n"
"  --insert-mode plain|replace  insert as INSERT (default) or INSERT OR REPLACE\n"
"                               (reference cb_copy_on_access_ratio; -1 default)\n"
"  --min-record N               PRAGMA bf_min_record: base of the derived\n"
"                               size-class ladder (reference cb_min_record_size)\n"
"  --ops-per-txn N              --autocheckpoint N   --mmap N\n"
"  --seed N  --label S  --sut S  --json PATH\n"
"  --drop-cache  --no-latency  --read-txn\n"
"  --direct-io                  main db via O_DIRECT (bfdio VFS; plan H7)\n";

int main(int argc, char **argv){
  Config cfg;
  int i;
  const char *zCmd;

  if( argc < 2 ){ fputs(zUsage, stderr); return 2; }
  zCmd = argv[1];
  configDefaults(&cfg);

  for(i=2; i<argc; i++){
    const char *z = argv[i];
    const char *zVal = (i+1 < argc) ? argv[i+1] : 0;
#define NEEDVAL if( zVal==0 ) die("%s needs a value", z); i++;
    if( strcmp(z,"--db")==0 ){ NEEDVAL; cfg.zDb = zVal; }
    else if( strcmp(z,"--records")==0 ){ NEEDVAL; cfg.nRecords = atoll(zVal); }
    else if( strcmp(z,"--value-len")==0 ){ NEEDVAL; cfg.nValueLen = atoi(zVal); }
    else if( strcmp(z,"--key-spacing")==0 ){ NEEDVAL; cfg.nKeySpacing = atoi(zVal); }
    else if( strcmp(z,"--workload")==0 ){ NEEDVAL; parseMix(&cfg, zVal); }
    else if( strcmp(z,"--dist")==0 ){ NEEDVAL;
      /* Unknown names used to fall back to scrambled zipf SILENTLY: the
      ** 2026-09-30 m2_split campaign asked for "zipfraw", got "zipf", and ran
      ** its whole contiguous arm as a second copy of the scrambled one. */
      if( strcmp(zVal,"uniform")==0 ) cfg.dist = DIST_UNIFORM;
      else if( strcmp(zVal,"latest")==0 ) cfg.dist = DIST_LATEST;
      else if( strcmp(zVal,"zipf-raw")==0 || strcmp(zVal,"zipfraw")==0 ) cfg.dist = DIST_ZIPFRAW;
      else if( strcmp(zVal,"zipf")==0 ) cfg.dist = DIST_ZIPF;
      else die("--dist: want uniform, latest, zipf or zipf-raw, got %s", zVal); }
    else if( strcmp(z,"--theta")==0 ){ NEEDVAL; cfg.theta = atof(zVal); }
    else if( strcmp(z,"--scan-len")==0 ){ NEEDVAL; cfg.nScanLen = atoi(zVal); }
    else if( strcmp(z,"--seconds")==0 ){ NEEDVAL; cfg.seconds = atof(zVal); }
    else if( strcmp(z,"--ops")==0 ){ NEEDVAL; cfg.nOps = atoll(zVal); }
    else if( strcmp(z,"--warmup-seconds")==0 ){ NEEDVAL; cfg.warmupSeconds = atof(zVal); }
    else if( strcmp(z,"--warmup-ops")==0 ){ NEEDVAL; cfg.nWarmupOps = atoll(zVal); }
    else if( strcmp(z,"--bf-cache-bytes")==0 ){ NEEDVAL; cfg.bfCacheBytes = atoll(zVal); }
    else if( strcmp(z,"--page-cache-bytes")==0 ){ NEEDVAL; cfg.pageCacheBytes = atoll(zVal); }
    else if( strcmp(z,"--synchronous")==0 ){ NEEDVAL; cfg.zSync = zVal; }
    else if( strcmp(z,"--journal")==0 ){ NEEDVAL; cfg.zJournal = zVal; }
    else if( strcmp(z,"--group-commit")==0 ){ NEEDVAL; cfg.nGroupCommit = atoi(zVal); }
    else if( strcmp(z,"--promotion")==0 ){ NEEDVAL; cfg.nPromotion = atoi(zVal); }
    else if( strcmp(z,"--adaptive-full")==0 ){
      NEEDVAL; cfg.nAdaptiveFull = atoi(zVal);
    }
    else if( strcmp(z,"--read-record-cache")==0 ){
      NEEDVAL; cfg.nReadRecordCache = atoi(zVal);
    }
    else if( strcmp(z,"--scan-promotion")==0 ){
      NEEDVAL; cfg.nScanPromotion = atoi(zVal);
    }
    else if( strcmp(z,"--evict-target")==0 ){
      NEEDVAL; cfg.nEvictTarget = atoi(zVal);
    }
    else if( strcmp(z,"--min-record")==0 ){ NEEDVAL; cfg.nMinRecord = atoi(zVal); }
    else if( strcmp(z,"--copy-on-access")==0 ){ NEEDVAL; cfg.nCopyOnAccess = atoi(zVal); }
    else if( strcmp(z,"--insert-mode")==0 ){ NEEDVAL;
      if( strcmp(zVal,"replace")==0 ) cfg.bInsertReplace = 1;
      else if( strcmp(zVal,"plain")==0 ) cfg.bInsertReplace = 0;
      else die("--insert-mode: want plain or replace, got %s", zVal); }
    else if( strcmp(z,"--bf-cache")==0 ){ NEEDVAL;
      cfg.bfEnable = (strcmp(zVal,"on")==0 || strcmp(zVal,"1")==0) ? 1 : 0; }
    else if( strcmp(z,"--ops-per-txn")==0 ){ NEEDVAL; cfg.nOpsPerTxn = atoi(zVal); }
    else if( strcmp(z,"--autocheckpoint")==0 ){ NEEDVAL; cfg.nAutoCheckpoint = atoi(zVal); }
    else if( strcmp(z,"--mmap")==0 ){ NEEDVAL; cfg.nMmap = atoi(zVal); }
    else if( strcmp(z,"--seed")==0 ){ NEEDVAL; cfg.seed = strtoull(zVal,0,10); }
    else if( strcmp(z,"--label")==0 ){ NEEDVAL; cfg.zLabel = zVal; }
    else if( strcmp(z,"--sut")==0 ){ NEEDVAL; cfg.zSut = zVal; }
    else if( strcmp(z,"--json")==0 ){ NEEDVAL; cfg.zJson = zVal; }
    else if( strcmp(z,"--drop-cache")==0 ){ cfg.bDropCache = 1; }
    else if( strcmp(z,"--no-latency")==0 ){ cfg.bNoLatency = 1; }
    else if( strcmp(z,"--read-txn")==0 ){ cfg.bReadTxn = 1; }
    else if( strcmp(z,"--direct-io")==0 ){ cfg.bDirectIo = 1; }
    else if( strcmp(z,"--help")==0 ){ fputs(zUsage, stderr); return 0; }
    else die("unknown option %s", z);
#undef NEEDVAL
  }

  if( cfg.nKeySpacing < 2 ) die("--key-spacing must be >= 2 (inserts need gaps)");
  if( cfg.nValueLen < 8 ) die("--value-len must be >= 8");

  if( strcmp(zCmd,"load")==0 ) return cmdLoad(&cfg);
  if( strcmp(zCmd,"run")==0 ){
    if( cfg.seconds==0 && cfg.nOps==0 ) cfg.nOps = 100000;
    return cmdRun(&cfg);
  }
  fputs(zUsage, stderr);
  return 2;
}
