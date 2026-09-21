# BF-Tree v2 — reference-parity plan

**Written 2026-09-16.**  Companion to `BF_TREE_V2_PLAN.md` (the route through
Phases 0–4).  This document covers one question only: **what does
`../bf-tree` and the VLDB'24 paper do that this fork does not**, and in what
order should we close it.

The project rule this plan serves: *stay as close to the reference
implementation and the paper as SQLite's own semantics allow.*  Where we
deviate, the deviation is named here, with the reason.

---

## 0. Why this plan starts with the benchmark, not with features

The 2026-09-16 `steady` campaign is the most careful read measurement this
repo has produced, and it reports **1.06x / 1.02x** against stock at
larger-than-memory zipf 0.9 / 0.99.  The paper reports a large win on the same
shape of workload.  Per the standing rule — *if we measure a slowdown, the null
hypothesis is that OUR integration is wrong* — that gap was audited before any
new feature was proposed.  Three findings came out, and they reorder the work.

### 0.1 The record cache was measured at half its configured size

`runner.py` rounded the ring **down** to a power of two inside a fixed budget.
A 256 MiB budget with an 8 MiB page-cache floor asks for a 248 MiB ring, gets
**128 MiB**, and the leftover 128 MiB is handed to the page cache.  Every
`ltm_steady` row in that campaign carries `bf.buffer_size = 134217728`.

So the totals were equal — the gate's only check — while the component under
test ran at half size and the surplus went to the component Bf-Tree exists to
replace.  Fixed in **A1** below.

### 0.2 Our workload is not the paper's workload

`benchmark/src/bench_bftree.rs` and `benchmark/src/common.rs`, against
`bench/harness/configs/steady.json`:

| | reference (`bench_bftree.toml`) | ours (`steady.json`) |
|---|---|---|
| record | key 16 B, **value = the key** → ~32 B | key + **100 B value** → ~116 B |
| zipf | `rand_distr::Zipf` rank used **directly as the key** — hot keys contiguous | **scrambled** through FNV (`bfbench.c:610`) — hot keys scattered |
| cache : data | 1 GiB : 3.2 GB ≈ **30 %** | 256 MiB : 6.8 GB ≈ **3.8 %** |
| threads | **30** | **1** (no `pthread` in `bfbench.c`) |

Each row independently shrinks the effect being measured.  Record size alone is
3.6x: a 32 B record against a 4 KiB leaf is a ~128x memory-amplification
argument, a 116 B record is ~35x.  Scrambling is why our mini-pages hold
**1.26 records each** (`live_mini_pages` 414 574 vs `cached_records` 522 780) —
we pay a mini-page header + meta + size-class round-up per *single* record.

Note this cuts both ways, and the honest report keeps both: YCSB scrambles, so
our config is the more conservative one, and a mechanism that needs 32 B records
and clustered hot keys to win is a *finding*, not a failure.  What is not
defensible is reporting only the adversarial config while citing the paper's
numbers as the target.

### 0.3 The per-record cost of the ring is 2.16x, and it is the capacity limit

`mini_page_bytes / cached_records` = **250 B to cache a 116 B record**.  Our
size classes are a fixed 64/128/256/…/4096 doubling (`bf_cache.h:43`); the
reference *derives* them from the record size —
`2^k · (min_record + sizeof(LeafKVMeta)) + sizeof(LeafNode)`, cache-line
aligned (`tree.rs:222-250`) — which for this shape yields a 192 B class where
we take 256.

Put together: a 128 MiB ring at 250 B/record holds the 522 k records we
measured, against stock's ~65 k cached leaf pages.  **8x the hot-set coverage
buys ~12 points of hit rate**, because zipf 0.9 over 60 M items has a fat tail.
That is the arithmetic behind "why don't we stand out on zipf": not one missing
feature, but a configuration in which the mechanism has little room to pay.

---

## Stage A — make the comparison the paper's comparison

No feature in Stage B can be evaluated until these land.

### A1. Fix the budget arithmetic  *(done — see `split_memory()`)*
`runner.py` now rounds the ring **up** and raises the total to match, so
`budget_bytes` is a floor rather than a ceiling and every SUT in a cell still
gets exactly the same bytes (`ring + page_floor`).  `bf_ring_round` keeps
`"down"` and `"up"` for reproducing older results.  Two new guards:

* the split is printed once per config before the campaign starts;
* every BF run asserts `bf.buffer_size` equals the ring it was handed, and a
  `strict` config aborts on mismatch.  Nothing checked this before, which is
  how a whole campaign ran at half size in silence.

`steady` goes from a 128 MiB ring to 256 MiB.  **Re-run it: this is the
cheapest experiment in the document.**

### A2. Replicate the reference's workload shape
Add a `paper` dataset and config to the harness:

* `--value-len 16` (the reference stores the key as the value);
* a new `--dist zipf-raw`: sampler rank used as the key with no FNV scramble.
  **Keep scrambled `zipf` as a separate, equally-reported mode** — the two
  answer different questions and the thesis needs both;
* 100 M records, budget ≈ 30 % of the data size.

`steady` stays as the adversarial case.

### A3. Multi-threaded readers — **re-scoped twice; this is the correct version**

The paper's 30 threads are not decoration: with many readers in flight a cache
hit removes queueing, not just one request, which is a large part of why a hit
is worth more there than here.

This item was first written as "add `--threads N` to `bfbench.c`" — a harness
change.  It was then re-scoped as "the cache is a process global with no
locking, so concurrent reads race".  **Both were wrong, and the second was wrong
because it read dead code.**  What is actually true:

* A `BfCache` is created **per pager** — `sqlite3PagerOpenBfCache` (`pager.c:4269`)
  says so outright: *"one BfCache per pager is the correct architecture."*
  Each connection therefore owns its own mapping table, mini-pages and ring.
* `bfGlobalCache` / `sqlite3BfGetGlobalCache` (`bf_cache.c:74`, `:701`) look
  like a shared singleton but have **zero call sites**.  Dead code.
* So there is no cross-connection race today, because there is no sharing.

The real problem is the opposite of a race, and it is worse for fidelity:
**the reference runs 30 threads against ONE buffer pool; N connections here
allocate N independent rings, each of `bf_cache_size`** (`bf_cache.c:227`
calls `sqlite3BfCacheBufferSize()` per cache).  Threading the harness as-is
would either overspend the memory budget N-fold or give each thread 1/N of a
cache — and the equal-budget comparison is the foundation the whole harness
rests on.

**Revised A3, in order:**

* **A3a (engine): share one record cache per database file across connections.**
  This is the faithful architecture and it is what makes a thread axis mean
  anything.  It needs the lock, because the moment two connections touch one
  cache ordinary reads collide: `sqlite3BfMiniPageSearch` sets the REF bit with
  a read-modify-write on `valueLenAndRef` — the same `u16` that carries the
  value length and the WAL-logged flag, so a lost update corrupts a length, not
  a hint — and `sqlite3BfMapLookup` walks a batch directory that
  `bfMapEnsureCapacity` reallocs under it.

  *Done:* `BfCache.mutex`, recursive, allocated and freed with the cache, with
  `bfCacheEnter`/`bfCacheLeave` helpers that compile out when
  `SQLITE_THREADSAFE=0`.  No inversion to design around — `bf_cache.c` never
  calls into the btree (flushes are driven the other way, from
  `bfFlushOneMiniPage`), and `evictCallback` refuses dirty pages rather than
  flushing them, so the critical section holds no I/O.

  *Not done, and the hard part:* the BF API hands out raw pointers into cache
  state (`sqlite3BfMapLookup` returns a `BfMapEntry*`), so the lock must span
  the **caller's** use of them.  The scope is the **28 `sqlite3BfBtree*` entry
  points** called from `btree.c` (71 call sites), not the leaf functions —
  locking the leaves would give false confidence.  Two of those entry points,
  the forward and reverse merge scans, retain mini-page pointers **across**
  btree steps and need a pinning design before any lock makes them safe.
  Plus the lifecycle change itself: one cache per file rather than per pager,
  with refcounting.

* **A3b (harness).** `--threads N`: per-thread connection, prepared statements,
  RNG and histograms; zipf table and Feistel permutation shared read-only after
  init; histograms merged at the end.  Writers keep serializing
  (`busy_timeout`), which stays a documented non-transfer.

**A3a is a project, not a task — larger than all of Stage B put together.**  The
rest of Stage A (A1, A2, A4) is independent of it and is done, so the `paper`
config runs single-threaded now and gains the thread axis later.  Treat
single-threaded `paper` numbers as a floor: they omit the queueing effect that
is part of why the paper's hit is worth what it is.

### A4. Report the per-record ring cost
`mini_page_bytes / cached_records` becomes a first-class column in `report.py`,
beside `cached_records` and `evictions`.  It decides every capacity question
and we have been computing it by hand.

**Gate for Stage A:** `paper` config, budget honoured, `cached_records`
demonstrated flat against a WARMUP axis (not merely reproducible across
repeats — every repeat warms for the same duration, so two runs that are both
too short agree perfectly), and the thread axis once A3a lands.  That number is
the honest "does it transfer" answer and the one the thesis should quote.

---

## Stage B — port the missing mechanisms

Dependency order.  Each lands behind an ablation switch in the existing
`SQLITE_BF_NO_*` style so the campaign can attribute the effect.

### B1. Reference-derived size classes — **DONE, and it required a PRAGMA**

`sqlite3BfInitSizeClasses` (`bf_mini_page.c`) implements the reference's
derivation (`tree.rs:222-250`):
`class(k) = 2^k * (base + sizeof(BfKVMeta)) + sizeof(BfMiniPage)`, cache-line
aligned.  It is now the single definition feeding both the `BfFreeList` and
`BfCache` copies of the ladder, which were duplicated — a disagreement between
them would have filed a block under one class and handed it back as another.

**The base is `PRAGMA bf_min_record`** (the reference's `cb_min_record_size`),
default 64.  It has to be configuration, and the reason is measured rather than
argued.  At a fixed base of 64, 4M rows under a 16 MiB ring:

| value_len | recs/page | B/record | cached |
|---|---|---|---|
| 32 | 5.4 | −2.5% | +1.4% |
| 64 | 2.3 | −12% | +11.7% |
| 100 | 1.5 | **−23.6%** | **+27.7%** |
| 200 | 1.2 | **+23.6%** | **−18.6%** |

A single fixed base is therefore a 28% capacity win on one workload and a 19%
capacity loss on another.  Shipping that as "the faithful ladder" would have
tuned the engine for our own benchmark and penalised the replication — backwards
for a change justified by fidelity.  The v200 row is why this is known: it was
included as a falsification test and it fired, within 1.4 points of the
predicted size.

Setting the pragma resolves it — v200 at `bf_min_record=208` (its full record:
8 B rowid key + 200 B value + 8 B meta) reads **262 B/record against the old
ladder's 263**, i.e. exact parity, from 325 at the default.

**But "set it to the record size" is NOT the rule.**  v100 is *better* at base 64
(182 B/record) than at its own record size of 108 (211), while v200 is far
better at 208 than at 64.  The relation is not monotonic, and the v100 case
shows up as a change in mini-page OCCUPANCY (1.75 vs 1.57 records/page) rather
than in class selection, which the class arithmetic does not explain.  Unknown
mechanism; measure per workload rather than computing the base.

| dataset | old ladder | base 64 | base = record size |
|---|---|---|---|
| v100 (116 B record) | 240 | **182** | 211 |
| v200 (216 B record) | 263 | 325 | **262** |

Caveat on all of the above: throughput moved 2-3% at most, and on the
larger-than-memory shape the warmup sweep showed 20% more cached records buying
*nothing* in hit rate.  **B1 is a capacity result, not yet a throughput
result.**  Judge it on `B/record`, and expect it to matter only where capacity
binds — which at 16 MiB / 4M rows it does (hit rate moved +0.8 pts at v100) and
at 256 MiB / 60M rows it does not.

### B2. Copy-on-access second chance **+** REF-bit record discard — one mechanism
This corrects the "REF-bit CLOCK eviction policy" item that `CLAUDE.md` carried:
**CLOCK is not in the paper or the reference.**  §4.1 has a second-chance
*region* — the tail-most 10 % of the ring, `cb_copy_on_access_ratio`, FIFO at
0.0 and strict LRU at 1.0 (Figure 14) — and the REF bit is consulted *only*
when a page is copied to the tail, to drop cold records
(`leaf_node.rs:1528`, `copy_initialize_to(..., discard_cold_cache: true)`).

We have the record-level half implemented and **dead**: `BF_COPY_REFERENCED`
exists (`bf_mini_page.c:799`) and both call sites pass `BF_COPY_ALL`.  The
earlier attempt built the region *without* the discard and measured
+1.2 pts hit / −3.6 % ops, and was reverted.  The discard is what makes the
copy pay for itself — it shrinks the page as it relocates.  Re-land both
together behind `PRAGMA bf_copy_on_access_ratio`, and sweep 0→100 % to
reproduce Figure 14.

### B3. Full-page cache (`BF_LOC_FULL`)
`upgrade_to_full_page` (`mini_page_op.rs:1407`) plus the paper's rule that a
mini-page past 2 KB merges with its leaf and becomes a 4 KB mirror.  We have
the enum value and one dead reference (`bf_mapping.c:418`); nothing creates one.
This attacks *admission*, which is what caps the hit rate.

**Interaction with A2, and the reason this comes after it:** under scrambled
zipf a full-page promotion admits ~34 records to serve 1 and is ring waste;
under the paper's contiguous zipf it is a large win.  Measure it where it is
supposed to work, and report both.

### B4. Separate scan promotion rate
`scan_promotion_rate` distinct from `read_promotion_rate` (`config.rs:38`).
One knob for both means scans either thrash the ring or point reads under-admit.

### B5. Eviction batching
Sweep toward `TARGET_EVICT_SIZE = 1024` B with a retry cap (`tree.rs:1012`),
replacing one block per call.  Only matters under pressure — but `saturated`
and `BF_CACHE_SIZE=262144` now produce real eviction, so it is finally testable.

---

## Stage C — measure and profile

### C0. Campaigns run at LETTER boundaries, not per change

Decided 2026-09-21, and it replaces this section's original "`configs/fixes.json`
with `--pre HEAD` after each of B1/B2/B3".  A steady-state read campaign costs
hours -- the larger-than-memory arm alone needs a 900 s warmup per cell
([[warmup-and-capacity-do-not-bind]]) -- and B is five mechanisms.  Paying that
five times serialises the work behind the measurement.

So there are **two tiers**, and mixing them up is the error to avoid:

**Per change — correctness only.**  The differential oracles (`stress.sh`,
`stress_buf.sh`, the `BF_CACHE_SIZE=262144` cycling-ring variant) plus
`fixes_smoke.json` as a *tripwire*: it catches an order-of-magnitude regression
and nothing subtler.  **Numbers from this tier are never quotable** -- no
warmup, no steady state, and `cached_records` still climbing.  Its only two
verdicts are "nothing exploded" and "stop, something did".

**Per letter — one campaign, with an ABLATION AXIS.**  This is what makes
batching free rather than lossy.  Every Stage B item lands behind a
`SQLITE_BF_NO_*` switch or a PRAGMA, by construction:

| item | switch |
|---|---|
| B1 derived size classes | `PRAGMA bf_min_record` |
| B2 copy-on-access + discard | `PRAGMA bf_copy_on_access`, `SQLITE_BF_NO_COPY_ON_ACCESS` |
| B3 full-page cache | `SQLITE_BF_NO_FULL_PAGE` (to be added) |
| B4 scan promotion rate | `PRAGMA bf_scan_promotion` (to be added) |
| B5 eviction batching | `SQLITE_BF_NO_EVICT_BATCH` (to be added) |

So ONE end-of-B campaign sweeping those knobs attributes each mechanism
*within* the campaign -- the same attribution the per-change A/Bs were buying,
one warmup instead of five, and every comparison within-campaign, which is the
only kind this repo trusts.  **A mechanism that cannot be switched off does not
get to land**: it would be unattributable at the letter gate.

Consequence to accept honestly: a regression is found later, with more changes
between it and the last known-good point.  The ablation axis is what makes that
bisectable at all, which is why the switch is a hard requirement and not a
nicety.

### C1. The rest

* One full `paper`-config campaign at the end, plus the Figure 14 ratio sweep
  and a promotion-rate sweep (both are named configs in `bench_bftree.toml`).
* Profile the read path **at the paper config**.  The current profile is of a
  workload where the cache rarely helps, so it profiles the wrong thing.
* Check occupancy (`cached_records / live_mini_pages`) before optimising any
  per-record loop.  That one division would have killed two of the 2026-09-15
  reverts in advance.

---

## Stage D — what counts as done

Named in advance so the result is not argued backwards.

1. **Reproduces.**  Paper config + honoured budget + N threads shows a clear
   win.  The mechanism transfers; `steady` then documents its sensitivity to
   record size and access clustering.  Best outcome and the most defensible
   claim.
2. **Reproduces partially**, residual traceable to buffered I/O.  Quantify it —
   bytes avoided vs. time saved is already instrumented — and state direct I/O
   as the known, deliberately unported prerequisite.
3. **Does not reproduce at parity of configuration.**  Only then is "structural
   difference from SQLite's B-tree" an honest conclusion, and it needs a
   code-level diff against `../bf-tree` behind it.

---

## Deliberately not ported

* **Direct I/O / io_uring backends** (`StdDirect`, `IoUringPolling`,
  `IoUringBlocking`).  SQLite's unix VFS does not use `O_DIRECT`, and the
  comparison this thesis makes is against gold-standard SQLite.  Porting it
  would change what we are comparing against.  It remains the honest
  prerequisite for claiming the paper's absolute numbers, and it is why both
  cache-retention experiments failed to pay for themselves: under buffered I/O
  an avoided miss is usually an OS page-cache hit.
* **CPR snapshots** (`src/snapshot.rs`).  SQLite has its own durability model;
  Phase 2's record-granular physiological WAL is the deliberate replacement.
* **Write concurrency.**  `BtShared` serialization stays; documented
  non-transfer.  Read concurrency is in scope — that is A3.
