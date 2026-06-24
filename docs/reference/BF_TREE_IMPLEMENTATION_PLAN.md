# Bf-Tree Implementation Plan (Prioritized)

Always build the project inside build/ to avoid polluting the src folder.

## Goal
Bring this SQLite fork from experimental BF plumbing to a correct, testable Bf-Tree-style implementation aligned with paper semantics and practical correctness requirements.

## Priority 0 (Blockers: correctness and safety)

### P0.1 Implement real merge-to-base-page persistence
**Why first:** Current merge/evict paths can mark dirty mini-pages clean without applying updates to base pages.

**Work:**
1. Implement merge logic that materializes dirty mini-page ops (insert/delete) into target SQLite leaf pages.
2. Ensure merge updates are durable through SQLite’s write path (pager/journal/WAL-consistent behavior).
3. Remove/forbid “mark clean only” paths unless merge succeeded.

**Primary files:**
- `src/bf_cache.c`
- `src/bf_btree.c`
- `src/btree.c`
- `src/bf_pager.c`

**Exit criteria:**
- Dirty mini-page data survives eviction/restart and matches base-page contents.

---

### P0.2 Replace pager stubs with real integration
**Why first:** BF behavior cannot be correct without pager ownership and recovery semantics.

**Work:**
1. Implement `sqlite3PagerUsesBfCache()` and `sqlite3PagerGetBfCache()` using real pager/pcache wiring.
2. Implement BF journal/WAL record paths or explicitly gate BF write-buffering off until equivalent durability is in place.
3. Implement replay/recovery handling for BF mini-page state.

**Primary files:**
- `src/bf_pager.c`
- `src/pager.c` (and related pager integration points)
- `src/bf_cache.c`

**Exit criteria:**
- BF cache ownership is pager-scoped and recovery behavior is deterministic.

---

### P0.3 Remove global-cache identity hazard
**Why first:** One global cache keyed only by page number is unsafe with multiple databases/attachments.

**Work:**
1. Replace global runtime cache usage with per-pager (or per-BtShared with proper file identity) cache instances.
2. Namespace all BF record/mapping lookups by file identity + page number (not page number alone).
3. Remove fallback paths that can cross-contaminate attached DBs.

**Primary files:**
- `src/bf_cache.c`
- `src/bf_btree.c`
- `src/bf_cache.h`
- `src/bf_pager.c`

**Exit criteria:**
- No shared-key collisions across attached databases/files.

---

### P0.4 Fix mapping lock concurrency with atomics
**Why first:** Current non-atomic lock updates are race-prone and can corrupt state.

**Work:**
1. Replace placeholder read/write lock assignments with atomic CAS-based lock transitions.
2. Audit lock/unlock and iteration paths for memory ordering and starvation concerns.
3. Add contention-focused stress tests.

**Primary files:**
- `src/bf_mapping.c`
- `src/bf_cache.c` (call sites)

**Exit criteria:**
- No data races in concurrent BF mapping operations; lock behavior is deterministic under stress.

## Priority 1 (Core behavior parity with Bf-Tree design)

### P1.1 Reintroduce safe write buffering in insert path
**Work:**
1. Re-enable insert buffering at a proven-safe hook point (not the prior unstable interception behavior).
2. Keep strict size/overflow guards and fallback to standard SQLite path when BF insertion is unsuitable.
3. Ensure failures propagate correctly (no silent success-shaped fallbacks).

**Primary files:**
- `src/btree.c`
- `src/bf_btree.c`
- `src/bf_cache.c`

**Exit criteria:**
- Point inserts are BF-buffered where valid, with no hangs/regressions.

---

### P1.2 Wire promotion and phantom caching into real read flow
**Work:**
1. Integrate promotion (`BFOP_CACHE`) after disk reads when policy allows.
2. Integrate phantom caching (`BFOP_PHANTOM`) on not-found point lookups.
3. Ensure lookup semantics remain SQLite-correct for all cursor/key modes.

**Primary files:**
- `src/btree.c`
- `src/bf_btree.c`

**Exit criteria:**
- Repeated point-lookups show real mini-page hit behavior and correct not-found semantics.

---

### P1.3 Correct range-scan policy behavior
**Work:**
1. Replace unconditional scan-time merge calls with policy-based behavior aligned to Bf-Tree intent.
2. Trigger merge/full-page transition only when thresholds or scan heuristics require it.
3. Prevent repeated per-step overhead in `Next` from forcing unnecessary merges.

**Primary files:**
- `src/btree.c`
- `src/bf_btree.c`
- `src/bf_cache.c`

**Exit criteria:**
- Range scans remain efficient without defeating point-cache benefits.

## Priority 2 (Observability, validation rigor, and hardening)

### P2.1 Make stats semantically correct
**Work:**
1. Redefine hit/miss counters so they represent actual BF read serving outcomes.
2. Separate probe/internal counters from user-facing efficacy counters.
3. Update `PRAGMA bf_cache_stats` to expose meaningful metrics.

**Primary files:**
- `src/bf_btree.c`
- `src/bf_cache.c`
- `src/bf_config.c`

**Exit criteria:**
- Reported stats match observed behavior and are suitable for validation.

---

### P2.2 Build a correctness test matrix
**Work:**
1. Add focused tests for point read/write/delete, phantom behavior, scan transitions, merge durability, restart recovery, and multi-db attach cases.
2. Add concurrency stress tests for mapping/cache operations.
3. Add regression tests for previously observed insert-path hangs.

**Primary areas:**
- `test/` BF-focused scenarios and SQL-level reproducible scripts.

**Exit criteria:**
- Failures reliably catch prior mismatch classes before optimization work.

---

### P2.3 Documentation and operational guardrails
**Work:**
1. Document what BF mode currently guarantees (and what is disabled/gated).
2. Provide validated runbook commands for enabling BF and interpreting stats.
3. Keep PROGRESS + plan aligned as milestones close.

**Primary files:**
- `README.md` (or BF section)
- `PROGRESS.md`

## Suggested execution order
1. P0.1 → P0.2 → P0.3 → P0.4  
2. P1.1 → P1.2 → P1.3  
3. P2.1 → P2.2 → P2.3

## Definition of “ready for deeper analysis”
- Merge is durable and correct.
- Pager/recovery paths are implemented (or unsafe modes explicitly disabled).
- Cache identity is scoped safely per database.
- Concurrency primitives are atomic and stress-tested.
- Metrics reflect real BF behavior.
