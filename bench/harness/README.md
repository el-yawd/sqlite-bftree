# BF-Tree v2 benchmark harness

The measurement campaign the plan calls Phase 4. It exists to answer one
question honestly — **does a record cache beat a page cache per byte of memory,
and does a record-granular WAL beat page-image logging per commit?** — and to
make a wrong answer visible instead of comfortable.

```
bfbench.c        C driver: prepared statements, per-op latency, block I/O
build_suts.sh    builds every SUT from ONE amalgamation (flags are the only diff)
runner.py        matrix expansion, memory fairness, cgroup isolation, manifests
report.py        results.jsonl -> RESULTS.md
configs/         smoke.json (6 min sanity), full.json (the campaign)
```

## Quick start

```bash
sh bench/harness/build_suts.sh --all         # or without --all for stock/bf/bf_ro
python3 bench/harness/runner.py bench/harness/configs/smoke.json    # ~6 min
python3 bench/harness/report.py bench/harness/results/smoke

python3 bench/harness/runner.py bench/harness/configs/full.json     # ~4-5 h
python3 bench/harness/report.py bench/harness/results/full
```

To measure **the effect of a change to our own `src/`**, add a `bf_pre` SUT
built from the revision you are comparing against and run the A/B config:

```bash
sh bench/harness/build_suts.sh --pre HEAD                            # + bf_pre
python3 bench/harness/runner.py bench/harness/configs/fixes_smoke.json  # ~2 min plumbing check
python3 bench/harness/runner.py bench/harness/configs/fixes.json        # ~1.5 h A/B
python3 bench/harness/report.py bench/harness/results/fixes
```

`--dry-run` prints the expanded matrix and a time estimate without running it.
`--only NAME` runs one experiment; `--suts bf,stock` overrides the SUT list.

## The requirements, and how each is met

A benchmark is only worth running if it can produce a result you would not have
wanted. These are the properties that make that possible.

### 1. Measure operations, not one query

The previous harness (`bench/read_bench.py`) drove the CLI with a giant SQL
script and measured a single JOIN. That reports one number, includes SQL text
parsing in the measured region, and cannot separate warm from measured. The
driver here links the amalgamation, prepares each statement once, and times
every individual operation with `CLOCK_MONOTONIC`, into an HDR-style histogram
(7 significant bits, ~0.8% error) — so p50, p99, p99.9 and p99.99 are real,
not inferred from a mean.

### 2. Equal memory, differently spent

`runner.py` gives both engines the identical byte budget:

| SUT | page cache | record cache |
|---|---|---|
| `stock` | budget | — |
| `bf` | `bf_page_cache_floor` | budget − floor |
| `bf_off` | budget | disabled via `PRAGMA bf_cache=off` |

The floor is not generosity: the BF build still routes interior nodes and every
non-BF page through SQLite's pager, so giving it zero page cache would be a
strawman. Totals match exactly, and the split lives in one function
(`build_argv`) so it can be checked in one place.

**The floor is a per-run axis, and the `page_cache_floor` experiment sweeps it.**
It has to be, because until 2026-08-26 it could not be: `bfCacheResizeHash` in
`src/bf_cache.c` fixed the pcache2 bucket count at 256 for the life of the
cache, so a BF build with a real page cache walked `nMax/256`-long chains on
every `xFetch` — measured at 43k vs 486k ops/s against stock at a 256 MiB page
cache, a 13× penalty that vanished at the 8 MiB floor. Every BF run in the
2026-08-25 campaign therefore sat at 8 MiB, which quietly turned "equal memory
budget" into "stock gets a page cache, BF does not". The hash now doubles
whenever `nPage` reaches `nHash`, and the split is a variable rather than an
assumption.

`memmax` accepts `"auto"` (2× budget + 256 MiB). A *fixed* cap across a budget
sweep is a confound: at budget=512M under a 1 GiB cap the engine owns half the
cgroup and the OS page cache is squeezed out, so both SUTs got *slower* as the
budget rose. `auto` holds the engine at a constant fraction of the cap so the
axis varies engine cache size and nothing else.

`bf_off` disables the **record cache** only. Insert buffering and the
record-granular WAL are compile-time (`SQLITE_BF_INSERT_BUFFERING`) and stay
active, so a `bf_off` write run still writes far less than stock. It is a valid
control for **read** workloads only, which is the only place the configs use
it; use `bf_ro` (Phase 1 build, no insert buffering) as the write-path control.
Two ordering rules follow from how the pragmas work, and both fail silently:
`PRAGMA bf_cache` must be issued **before** anything creates a pager (it swaps
the global pcache methods, which only apply to caches created afterwards),
while `bf_cache_size` / `bf_group_commit` / `bf_promotion_rate` must be issued
**after** `PRAGMA journal_mode` (which reopens the pager and drops settings made
before it). `bfbench.c` does both; a knob that reads back correctly but has no
effect is almost always this.

### 3. Larger than memory, actually enforced

Each run executes in a transient systemd scope with `MemoryMax` and
`MemorySwapMax=0`. A cgroup v2 memory cap covers the **page cache** too, so the
OS cannot hold the dataset and hide the difference. The default campaign uses a
~7 GiB dataset under a 1 GiB cap — 7× oversubscribed. On an uncapped box this
whole comparison is meaningless, which is why earlier numbers in this repo were
not trustworthy.

The driver also `POSIX_FADV_DONTNEED`s the database before each run. This
matters more than it looks: cgroup v2 charges a page-cache page to whoever
faulted it in **first**, so pages left behind by the load phase stay free to the
capped process and quietly defeat `MemoryMax`.

### 4. I/O counted where it actually happens

The driver reads its own `/proc/self/io` from inside the scope, giving
block-layer `read_bytes`/`write_bytes` for the benchmark process. The old
harness used `getrusage(RUSAGE_CHILDREN)` in the parent, which reads **0** under
`systemd-run` because the process is no longer our child — a documented
known-bad number that made every capped run look like it did no I/O. Bytes are
the hardware-independent result; wall time is the hardware-dependent one, and
both are reported.

### 5. A fast wrong answer is not a win

Values are generated deterministically from the key, so every read verifies what
it got back. Reads that return the wrong bytes, or miss a row that must exist,
increment `read_errors`/`read_misses`; `report.py` **excludes** any such run
from every table and lists it in a Correctness section. This is the guard
against a caching bug that "wins" by not returning data.

### 6. Identical starting state

Each dataset is loaded **once, by the stock binary**, and kept read-only. Every
run that writes gets a fresh copy, so no run inherits another's tree shape, WAL,
or free list. Loading with stock also guarantees the base tree is plain SQLite
format, so nothing about the starting state can favour the fork.

### 7. Reproducibility

`manifest.json` records compiler and version, the exact flag set, the
amalgamation's SHA-256, the git revision **and whether the tree was dirty**, plus
kernel, CPU model, governor, RAM, filesystem, mount options and device.
`report.py` reprints all of it, and turns the awkward parts into caveats
attached to the tables.

A config may set `"strict": true` (as `full.json` does) to make the two
conditions that silently ruin a campaign — a **dirty working tree**, so the
recorded revision does not identify the code under test, and a
**non-`performance` CPU governor** — hard errors instead of notes. `--force`
overrides. Non-strict configs still print the warnings.

### 7b. Counters that measure what they are named

`PRAGMA bf_cache_stats` reports `mini_page_hits`/`mini_page_misses` for the
**record** cache and `page_cache_hits`/`page_cache_misses` for the pcache2
layer, and `report.py` shows them as separate `rec hit%` and `pg hit%` columns.
They used to share the mini-page counters, because a pager's `BfCache` *is* its
pcache2 instance (`sqlite3PagerOpenBfCache`) and `bfCacheFetch` bumped
`nMiniPageHit` on every page hit. The published hit rate was therefore mostly a
page hit rate — which is why it sat at a flat 56.7% across an 8× sweep of
`bf_cache_size`, a number that cannot depend on the record cache at all. Reports
generated from result files that predate the split are labelled as such rather
than re-plotted.

### 8. Repetition and honest spread

Every cell runs N times (5 in the campaign) and the tables report the **median**,
with spread as `(max−min)/median` in its own column. A ratio quoted next to a
40% spread is not a result, and the table says so rather than hiding it.

`report.py` now **bolds** any cell whose spread exceeds 15% and counts them in a
"Reading these numbers" section, because saying so in prose was not enough: the
2026-08-25 campaign had cells at 80% and 120% spread whose ratios were written
up as findings. Every noisy cell came from a write-heavy experiment
(`durability_bf` 8 of 9, `record_size` 7 of 8, `update` and `insert` 2 of 2), so
those now run 30 s with an 8 s warmup instead of 20 s / 5 s — a longer window
straddles more WAL-checkpoint cycles, which is where the variance came from.

Ratios are oriented so **>1 always means BF is better** — throughput divides
bf by stock, latency and bytes divide stock by bf. Mixing orientations between
tables is how benchmark reports accidentally claim wins.

### 9. btrfs is not allowed to write the conclusion

This tree is on btrfs, whose copy-on-write and checksums add write amplification
of their own. The runner sets `chattr +C` (nodatacow) on the work directory so
database files are written in place, verifies it took, and records
`nodatacow: true/false` in the manifest. If it failed, `report.py` prints a
caveat saying write volumes are upper bounds. Copies are plain, never
`--reflink`: a reflinked file takes a COW fault on the first write to every
extent, landing exactly in the middle of the write experiments.

### 10. Before/after is a SUT, never a second results directory

`--pre REV` builds an extra SUT named `bf_pre` from a **second amalgamation**,
generated in a git worktree at `REV` (kept outside the repo so it cannot dirty
the tree), compiled with this harness's `bfbench.c` and the identical `COMMON`
flags. Only `src/` differs.

It has to work this way. Stock — identical code, identical machine — reported
9913, 11652 and 8102 ops/s across three campaigns, a 23% spread; a before/after
read off two results directories is measuring the afternoon, not the patch.
`bf_pre` runs interleaved with `bf` under the same cgroup, the same governor and
the same page-cache state, and `report.py` prints a `bf/bf_pre` ratio block
next to the `bf/stock` one wherever both are present.

Two things to keep in mind when reading a `bf_pre` row:

* **Counters that postdate `REV` are absent, not zero.** The space gauges
  (`cb_capacity`, `cached_records`, …) and the record/page hit-rate split are
  recent; for an older build `rec hit%` is the old conflated counter that also
  counted page hits, so it is not comparable. `report.py` says so under any
  table where it applies. `buffered%` and frames/commit exist in both.
* **An older build may not honour a knob at all**, which is often the very
  point of the comparison — but it also means it may sit *under* the memory
  budget rather than at it, and under a cgroup that cap covers the page cache,
  so the smaller engine gets more file cache for free. Read the counters before
  the wall clock.

### 11. Equal budgets, down to the rounding

The record cache's ring is a power of two by construction
(`bf_circular_buffer.c` masks with `capacity-1`) and `PRAGMA bf_cache_size`
rounds a request **up** to reach one. So `budget - floor` is not what BF gets:
a 64 MiB budget with an 8 MiB floor asked for 56 MiB, got a 64 MiB ring, and
spent 72 MiB — 12.5% more than stock, along the exact axis `point_read` sweeps.
This was invisible while `bf_cache_size` had no effect (every ring was 8 MiB,
whatever was asked). `build_argv` now rounds the ring **down** to a power of two
and gives the remainder to the page cache, so the totals match exactly and the
resulting split is explicit; `"bf_ring_round": "up"` in a config restores the
overshooting behaviour if you need to compare against numbers recorded before
this.

## Workloads, and the claim each one tests

| experiment | mix | tests |
|---|---|---|
| `point_read` | `read=100`, 4 cache sizes | the paper's ~2× point-read win |
| `page_cache_floor` | `read=100`, floor 8M → 128M | how BF should split its budget between pages and records |
| `skew` | `read=100`, uniform → Zipf 0.99 | whether the win depends on skew |
| `scan` | `scan=100`, lengths 8/32/128 | the paper's ~2.5× scan win |
| `negative_read` | `negative_read=100` | phantom / negative-search caching (§5.6) |
| `update`, `insert` | 100% writes | the paper's ~6× write win |
| `workload_mix` | read/update 100:0 → 0:100 | the paper's workload sweep |
| `ycsb_a/b/d/e/f` | standard YCSB mixes | comparability outside this repo |
| `durability_bf/stock` | `update=100` × synchronous × group-commit | **our** contribution: the record-WAL trade-off |
| `record_size` | `insert=100` × 32/64/100/200 B values | the write-back size cliff |
| `write_amp` | `insert=100`, autocheckpoint off | bytes on disk per commit |
| `overhead` | `bf_off` vs `stock` | cost of the fork with the feature off |

`configs/fixes.json` is the A/B of the 2026-09-07 record-cache fixes and runs a
subset of these against `bf_pre`: `cache_size` (did `bf_cache_size` start taking
effect), `record_size` (did the 64-96 B buffering cliff close), `update` (does
UPDATE buffer at all now), `promotion` (the still-open ring-space problem),
plus `scan`, `negative_read` and `write_amp` as controls. Each experiment
carries a `_tests` field naming the fix it interrogates and the column to read
first — usually a counter, not a throughput.

Keys are drawn YCSB-style: a Zipfian rank, then **scrambled through FNV-1a** so
the hot set is scattered across the file. Without the scramble, "skew" degrades
into "read the first few pages", which any page cache wins trivially.

Insert keys come from a **4-round Feistel permutation** over the gap slots
between loaded rowids. Being a bijection, it guarantees no insert ever collides
with an existing row — without tracking 60M used keys — while still scattering
inserts across the whole key space so each commit touches a different leaf. The
top half of the slot space is reserved for negative reads, so the two never
overlap.

## Direct I/O (`--direct-io`, config key `direct_io`; plan H7)

Under buffered I/O an avoided page read is usually an OS page-cache hit, so the bytes BF
saves barely move throughput.  `--direct-io` opens the main database through `bfdio`, a shim
VFS in `bfbench.c` that serves reads and page-aligned writes from a second `O_DIRECT`
descriptor via an aligned bounce buffer; locking, sync, WAL and shm stay on SQLite's own unix
file.  Both SUTs use it, so SQLite core and the comparison against stock are unchanged.

**Proven per run, never assumed** (btrfs accepts `O_DIRECT` and still serves compressed
extents from the page cache).  The driver's JSON `result.dio` carries the db file's resident
OS-cache pages before and after the measured phase (`mincore`); the runner warns, and the
report excludes the run, if they grow by more than 256 pages.  In a valid direct run
`io.read_bytes` equals `pcache.miss` x page size to the byte.  A filesystem that rejects
`O_DIRECT` kills the run rather than falling back.  Warm up in operations
(`warmup_ops`), not seconds, so both modes enter the measured phase with the same history.

## Known gaps

- **Single threaded.** The fork keeps SQLite's single-writer serialisation, so
  the paper's concurrency and scalability results are a documented non-transfer.
  The driver has no thread pool because there is nothing to measure yet.
- **`copy_on_access_ratio` is not sweepable.** The paper sweeps it and
  `BfConfig` holds it, but no `PRAGMA` exposes it — only `SQLITE_CONFIG_BFCACHE`
  does. Exposing a pragma would make this axis available; the driver
  deliberately does *not* pass an unknown pragma, because SQLite accepts unknown
  pragmas silently and it would look like a swept knob that never moved.
- **No O_DIRECT.** Isolation is by cgroup cap, not direct I/O. A shim VFS could
  open the main DB with `O_DIRECT` (the WAL cannot: its frames are
  `page_size+24`, so they are not sector-aligned) to match the paper's
  `StdDirect` backend more closely.
- **Rowid tables, one blob column.** That is v1's buffering scope. A schema with
  secondary indexes will see a smaller effect than these tables show.
