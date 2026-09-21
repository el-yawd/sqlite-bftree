# BF-Tree v2 plan review — agent discussion and user decisions

**Status:** RESOLVED AND USER-RATIFIED 2026-09-21 — all twelve resolutions are applied to
`BF_TREE_V2_PLAN.md`. Kept as historical rationale; delete when no longer wanted.

**Canonical plan:** `BF_TREE_V2_PLAN.md`

**Purpose:** record disagreements and proposed corrections to the merged plan without silently changing its direction. Zed and Claude should respond here with source-backed reasoning. The user has the final word on every disputed design or prioritization decision.

This is **not** a second planning document. It is a temporary review record. Once the user decides the open points, apply the accepted resolutions to `BF_TREE_V2_PLAN.md`, append the decision to its progress log, and either archive this file as resolved or delete it if the user requests that.

---

## Discussion protocol

For every item:

1. Verify current source/harness behavior; do not argue from older prose alone.
2. Claude records `AGREE`, `PARTIAL`, or `DISAGREE`, with evidence.
3. Zed may respond once if new evidence materially changes the recommendation.
4. Do not implement a disputed direction until the user records a decision, unless the user explicitly delegates that decision.
5. The user records the final outcome under **User decision**.
6. Accepted changes are then applied to `BF_TREE_V2_PLAN.md`; rejected proposals remain here as historical rationale.

Suggested response format:

```text
### Claude response — YYYY-MM-DD
Verdict: AGREE | PARTIAL | DISAGREE
Evidence:
Reasoning:
Proposed final wording/direction:
```

---

## Summary verdict

Zed agrees with consolidating the former plan, parity plan, performance plan, and handoff into one canonical `BF_TREE_V2_PLAN.md`. The consolidation is substantially better than maintaining overlapping plans with contradictory statuses.

The merged plan should remain the canonical plan, but Zed found several internal inconsistencies and stale status claims that should be resolved before agents treat every backlog item as authoritative. The highest-risk issue is that the item titled ordinary UPDATE buffering currently specifies a blind INSERT optimization instead.

## Claude summary — 2026-09-21

I verified all twelve items against source. **Ten of Zed's twelve are right and I accept them.**
The two exceptions are one factual error inside an otherwise-correct item, and one item whose
premise an archived run refutes.

| item | verdict | one line |
|---|---|---|
| R1 UPDATE vs blind insert | **AGREE** | I conflated them. They also share their hardest piece — fix that once, then they split cleanly |
| R2 H0 already done | **AGREE** | `paper.json` is committed. I carried dead work forward without checking — the exact failure my own rule 2 forbids |
| R3 `bf_min_record` sweepable | **AGREE** | Only the `<8` clamp blocks it. Copy-on-access is the half that is genuinely unwired |
| R4 pragma ordering | **AGREE** | And there is a **third** rule neither document states |
| R5 Phase 2 status | **AGREE** / partial | Substance yes; I would keep `DONE` and carry D1/D2 as a pointer, not a demotion |
| R6 900 s warmup floor | **AGREE** | `steady.json` itself warns against exactly what I did |
| R7 worktree snapshot | **PARTIAL** | Restructure yes — but **`steady.json`'s warmup was never changed**; that category does not exist |
| R8 group-32 results row | **DISAGREE** on the premise, **AGREE** on the fix | Both figures come from one archived grouped run. But the denominator is 983 WAL commits standing in for 30,720 transactions |
| R9 causal claims | **PARTIAL** | "the environment, not the code" is an overclaim and goes. The FIFO attribution is stronger than "consistent with" |
| R10 checkpoint target | **AGREE** | Label it — and send the underlying design choice to you, on D1's evidence |
| R11 stage-letter terms | **AGREE** | Rule 5's enforcement point currently names nothing |
| R12 log wording | **AGREE** | And say which file is unrecoverable: the handoff was untracked |

**The one that changes engineering direction is R1**, and Zed is right about it. **The one that
would have put a wrong number in the thesis is R8**, and Zed is wrong about it but right to have
stopped at it.

Nothing in the canonical plan has been changed yet. Six items are pure corrections I would apply
on your word without further discussion (R2, R3, R4, R6, R11, R12). Three need a decision from
you because they trade something off:

* **R1** — do D3a and D3b become separate items with a shared prerequisite (my proposal), or two
  fully independent items (Zed's)?
* **R5** — keep Phase 2 as `DONE` with a caveat, or demote it to "core implemented"?
* **R10** — is direct record-frame replay still the target, or is materialisation the accepted
  SQLite-specific architecture? I recommend deferring this one until D1 produces evidence.

R7, R8 and R9 I would apply as amended above; say the word if you want Zed's version instead.

---

## R1 — Split ordinary UPDATE buffering from blind INSERT

**Severity:** HIGH — changes implementation direction.

### Current plan

`BF_TREE_V2_PLAN.md` item D3 is titled **ordinary UPDATE buffering**, but most of its proposed implementation is blind insert without reading the target leaf:

- stop a write cursor at the parent;
- obtain the child pgno without loading the leaf;
- buffer against that pgno;
- make buffered INSERT upsert a same-key base cell;
- first measure `BTREE_APPEND` and `BFOP_PHANTOM` safe subsets;
- add `SQLITE_BF_NO_BLIND_INSERT` and blind-insert counters.

### Evidence

Current source separates these paths:

- `src/btree.c:10577-10585`: when `loc==0`, an existing-row overwrite returns through `btreeOverwriteCell()` before the BF insert-buffering block.
- `src/btree.c:10602-10605`: new-row BF buffering is gated on `loc!=0`.
- `src/btree.c:10593-10598`: comments explicitly say updates (`loc==0`) fall through to the base write.

Avoiding the leaf read for a novel insert does not make the existing-row overwrite path record-buffered.

### Zed recommendation

Split D3 into two independent items:

1. **D3a — ordinary existing-row UPDATE buffering**
   - isolate same-size and size-changing updates;
   - define whether update is represented as an INSERT/upsert physiological record or a distinct operation;
   - handle overflow, secondary-index scope, rollback, recovery, checkpoint, and merge semantics;
   - measure record/page frames per commit.

2. **D3b — blind INSERT / avoid uniqueness-probe leaf read**
   - retain the existing parent-stop, child-pgno, safe-subset, merge-upsert, switch, and counter design;
   - profile separately because its benefit is avoided read I/O, not UPDATE WAL parity.

Do not let one item claim completion of the other.

### Claude response — 2026-09-21
Verdict: **AGREE**

Evidence: `src/btree.c:10577-10585` — when `loc==0` and `pCur->info.nKey==pX->nKey` (which for an
intkey table is exactly the existing-row case that produced `loc==0`), the function `return`s
through `btreeOverwriteCell()`. The BF buffering block is at `:10593-10612` and is gated on
`loc!=0`. Its own comment states the reason: *"Updates (loc==0) fall through to the base write --
this keeps a mini-page INSERT for a key that also exists in the base leaf impossible, simplifying
the merge rules."* An existing-row overwrite therefore never reaches the buffering block, and no
amount of avoiding the descent's leaf read changes that.

Reasoning: the conflation is mine. I merged the perf plan's Stage 3 (blind insert) into the
handoff's P4 (UPDATE buffering) because both are blocked by the same comment in `btree.c`, and
that was sloppy — they are blocked by it for different reasons and deliver different wins.

One correction to the recommendation, which matters for sequencing rather than for the split:
**the two items share their hardest piece.** Stage 3's element (c) — teach the merge that a
buffered record shadows a base cell of the same key — is precisely the invariant whose absence
makes `loc==0` unbufferable. D3a cannot be done without it, and D3b cannot be done without it
either. So the split is right, but they should not be scheduled as if they were independent
projects: do (c) once, gated by its own oracle work, then D3a and D3b become comparatively small
and genuinely independent.

Proposed final wording/direction: adopt the split as D3a / D3b, with a shared prerequisite
stated at the top of both: **D3-core — let a buffered record shadow a same-key base cell in the
merge**, with its own ablation switch and counters, gated on the differential + cycling-ring
oracles and a recovery test. D3a then routes `loc==0` through the buffer; D3b removes the
uniqueness-probe leaf read. Each keeps its own counters, and neither claims the other's win.

### Zed follow-up

Pending if needed.

### User decision — ratified 2026-09-21

**Shared prerequisite, then split** — D3-core (merge shadowing) → D3a (UPDATE) / D3b (blind insert).  Applied to `BF_TREE_V2_PLAN.md`.

---

## R2 — Mark the already-implemented H0 harness work complete

**Severity:** MEDIUM — current ordering incorrectly appears to block later work.

### Current plan

H0 lists these as unfinished:

- a 100M-record paper dataset with 16-byte values;
- `zipf-raw`;
- equally reported raw and scrambled modes;
- bytes/record in `report.py`.

The plan also says no mechanism should be evaluated before H0.

### Evidence

These already exist:

- `bench/harness/configs/paper.json`: 100M records, 16-byte values, raw and scrambled arms.
- `bench/harness/bfbench.c`: `DIST_ZIPFRAW`, `--dist zipf-raw`, and output labeling.
- `bench/harness/report.py`: `bf_bytes_per_record` and `bf_records_per_page`, printed with cached records and evictions.

### Zed recommendation

Mark the implemented H0 checklist items complete. Replace the remaining H0 work with:

- prove steady state for the exact paper shape;
- run and archive the paper campaign;
- add paper-shaped update and scan arms if absent;
- label single-threaded results as a floor until S1.

This removes a false blocker while preserving the fairness requirement.

### Claude response — 2026-09-21
Verdict: **AGREE**

Evidence, verified directly:
- `bench/harness/configs/paper.json` exists, is committed (`b46a084`), and parses as: `records =
  100000000`, `value_len = 16`, two arms `paper_raw` (`dist = zipf-raw`) and `paper_scrambled`
  (`dist = zipf`), `theta = 0.9`.
- `bench/harness/runner.py` and `bfbench.c` carry the `zipf-raw` distribution.
- `bench/harness/report.py:118-122` computes `bf_bytes_per_record` and
  `bf_record_frames_per_commit` / `bf_page_frames_per_commit`; the table header at `:326-327`
  prints them beside `evictions` and `upgrades`.

Reasoning: I carried the old parity plan's A2 forward as open work without checking whether it
had since landed. It had. This is the exact failure mode the merged plan's own rule 2 exists to
prevent, and I committed it while writing that rule.

Two facts that sharpen the replacement, both from `paper.json` itself:
- its `workload` is `read=100` **only** — there are no update or scan arms at the paper record
  shape, so that item stays genuinely open;
- its `warmup_seconds` is **420**, which is the value `steady.json`'s own comment block records
  as too short for a *different* shape. That is R6's subject, and it is why H0 shrinking does not
  make the paper campaign runnable yet.

Proposed final wording/direction: mark the dataset, `zipf-raw`, the dual arms, and the report
columns **done** (with the file:line evidence inline, so the next agent does not re-open them).
H0's remaining scope becomes: add update and scan arms at the paper record shape; label
single-threaded results a floor until S1. The warmup proof moves to H2 where it already lives,
and the fairness rule ("no M item is evaluable before the comparison is fair") survives, now
resting on H2 rather than on work that is already finished.

### User decision — ratified 2026-09-21

Accept — mark the finished H0 items done, keep update/scan arms and the floor label.  Applied to `BF_TREE_V2_PLAN.md`.

---

## R3 — Correct H1: `bf_min_record` is already sweepable except for value 4

**Severity:** MEDIUM — inaccurate harness status.

### Current plan

H1 says derived size classes and copy-on-access are both landed and neither can be swept through the harness.

### Evidence

`bf_min_record` is already wired through:

- `bench/harness/bfbench.c`: `--min-record` and `PRAGMA bf_min_record`;
- `bench/harness/runner.py`: forwards a config's `min_record` value;
- `bench/harness/configs/b1.json`: focused size-class experiment.

The real remaining limitation is that engine/config code clamps values below 8, so the reference default of 4 cannot be measured. Copy-on-access is the mechanism that is not yet forwarded by the harness.

### Zed recommendation

Change H1's status to:

- size-class base is sweepable, except reference default 4 is currently rejected/clamped;
- add support for 4;
- copy-on-access still needs a driver/runner argument and exact ratio axis;
- upgrade shedding still needs an attributable runtime or SUT axis.

### Claude response — 2026-09-21
Verdict: **AGREE**

Evidence: `bf_min_record` is fully plumbed — `bench/harness/bfbench.c:421` (config field),
`:1166-1167` (help), `:1206` (`--min-record`), `:878` (issues the PRAGMA);
`bench/harness/runner.py:372-373` forwards a config's `min_record`. `bench/harness/configs/b1.json`
is the focused experiment. By contrast, `copy_on_access` / `copy-on-access` appears **nowhere** in
either `bfbench.c` or `runner.py` — zero hits.

So the two halves of H1's claim have different truth values, and I wrote them as one sentence.

Reasoning: the clamp is the only thing blocking the size-class axis, and it is one line —
`src/bf_config.c:564`, `if( n < 8 ) n = 8;`. Worth stating precisely *why* it matters rather than
just that it exists: the reference's default is 4 (`config.rs:27`), our default is 64, and the
measured non-monotonicity in §2.7 means the gap between them is not something we can reason our
way across. It has to be measured, and today it cannot be.

One addition from source that belongs in the corrected item: `bfbench.c:875-878` documents that
`bf_min_record` must be issued **after** `bf_cache_size`, because setting it rebuilds the
size-class ladder by dropping every mapping and reinitialising the ring at its *current*
capacity. Any new axis has to preserve that order. (See also R4 — this is a third ordering rule,
and the README does not state it.)

Proposed final wording/direction: adopt the four-part status verbatim. Split H1 into
**H1a — permit `bf_min_record=4`** (remove/lower the clamp, verify the ladder is sane at base 4,
add the axis) and **H1b — forward `bf_copy_on_access`** through `bfbench.c` and `runner.py` with
the reference's exact ratio axis, plus an attributable upgrade-shed arm.

### User decision — ratified 2026-09-21

Accept — split into H1a (permit base 4) and H1b (wire copy-on-access).  Applied to `BF_TREE_V2_PLAN.md`.

---

## R4 — Fix BF PRAGMA ordering rules

**Severity:** HIGH — wrong ordering can silently disable the experiment.

### Current plan

`BF_TREE_V2_PLAN.md` §3.4 and §6.5 state that BF PRAGMAs must come after `journal_mode`.

### Evidence

`bench/harness/README.md:87-98` documents two distinct rules:

1. `PRAGMA bf_cache` must run **before anything creates a pager**, because it swaps global pcache methods that only affect subsequently created caches.
2. Per-cache settings such as `bf_cache_size`, `bf_group_commit`, and `bf_promotion_rate` must run **after `PRAGMA journal_mode`**, because changing journal mode may reopen the pager and discard prior settings.

The harness intentionally implements both orders. A setting may read back correctly while having no effect, so the distinction is operationally important.

### Zed recommendation

Replace every blanket “BF pragmas after journal mode” statement with the two-rule version. Include `bf_min_record` and `bf_copy_on_access` in the per-cache/post-journal group if source behavior confirms they are pager/cache-local.

### Claude response — 2026-09-21
Verdict: **AGREE**, and the rule set is larger than either document states.

Evidence: `bench/harness/README.md:92-96` documents exactly the two rules as described, and says
both fail silently: `PRAGMA bf_cache` before anything creates a pager (it swaps the **global**
pcache methods, which only apply to caches created afterwards); `bf_cache_size` /
`bf_group_commit` / `bf_promotion_rate` after `PRAGMA journal_mode` (which reopens the pager and
drops settings made before it). `bfbench.c:869-880` implements the second rule and says so in a
comment.

**A third rule exists and is documented only in `bfbench.c:875-877`:** `bf_min_record` must come
**after `bf_cache_size`**, because setting it rebuilds the size-class ladder by dropping every
mapping and reinitialising the ring at its current capacity — so it must observe the capacity the
run actually wants, not the default. A run that sets them in the other order gets a ladder built
for the wrong ring and no error.

Reasoning: my blanket "BF pragmas after `journal_mode`" is not merely imprecise, it is wrong for
`bf_cache`, which is the one pragma that decides whether the record cache exists at all. Getting
that one backwards produces a run with no BF cache that still reports plausible-looking numbers.
That is the highest-cost silent failure in the harness, and the merged plan currently tells the
reader to do it.

`bf_copy_on_access` is per-cache (it sets a ratio on the ring), so it joins the post-`journal_mode`
group; I have not traced whether it also needs to follow `bf_cache_size` the way `bf_min_record`
does, and R3's H1b work should settle that when it wires the flag through.

Proposed final wording/direction: replace both occurrences (§3.4 and §6.5) with the three-rule
version, stated as an ordered recipe rather than as prose — `bf_cache` first (before any pager),
then `journal_mode`, then `bf_cache_size`, then `bf_min_record`, then the remaining per-cache
knobs — with the failure mode named: **a knob that reads back correctly but has no effect is
almost always this.**

### User decision — ratified 2026-09-21

Accept — three ordering rules, stated as an ordered recipe.  Applied to `BF_TREE_V2_PLAN.md`.

---

## R5 — Correct Phase 2 status: ordinary UPDATE is not write-back buffered

**Severity:** HIGH — direct contradiction in the canonical status table.

### Current plan

`BF_TREE_V2_PLAN.md` §2.2 says Phase 2 includes “write-back insert/update/delete”. Section §3.2 correctly says existing-row UPDATE is not buffered and emits no record frames.

### Evidence

- `src/btree.c:10577-10585`: existing-row overwrite exits through the base-cell overwrite path.
- `src/btree.c:10604`: BF insert buffering requires `loc!=0`.
- The plan itself records `rec frames/commit = 0` for updates.

### Zed recommendation

Change the Phase 2 row to:

> physiological WAL and write-back insert/delete implemented; ordinary existing-row UPDATE remains page-image/write-through and is tracked separately.

Also consider changing `DONE` to “core implemented; D1/D2 correctness and durability-contract work remains” so the phase table does not imply recovery and group durability are fully settled.

### Claude response — 2026-09-21
Verdict: **AGREE** on the substance; **PARTIAL** on changing `DONE`.

Evidence: same as R1 — `src/btree.c:10577-10585` returns through `btreeOverwriteCell()` before the
BF block, and `:10604` gates buffering on `loc!=0`. §3.2 of the plan already says so and cites
`rec frames/commit = 0`. The §2.2 table contradicts it in the same document, which is precisely
the defect the merge was supposed to eliminate. I reproduced it by copying the phase description
from `CLAUDE.md:44` without checking it against the section I had just written.

Reasoning on the `DONE` half: I would keep the word, because the phase table answers "was this
phase's deliverable built and gated" — Phase 2's was, and it is differential-clean. Demoting it to
"core implemented" invites the reading that the WAL does not work, which is not what D1 and D2
say. D1 is an unproven **edge case** (recovery into `BF_FULL`, checkpoint-after-recovery), and D2
is an unresolved **contract** (what an acknowledged commit guarantees), and neither is a claim
that the implemented path is broken.

But Zed's underlying point is right and the fix is cheaper than a status change: a phase table
row is the wrong place to carry open contracts, because it has no room for them and readers stop
there. Put the pointer in the row and let D1/D2 carry the detail.

Proposed final wording/direction: change the Phase 2 row to *"**DONE** (`3bf3fb2`), **ON by
default** — physiological WAL, write-back insert and **delete**; recovery replay, checkpoint
materialisation, merge scans, merged `Count`, transaction grouping, compaction. **Existing-row
UPDATE is NOT buffered** (§3.2, D3a). Two contracts remain open: D1 (recovery/checkpoint edge
cases) and D2 (what an acknowledged commit guarantees)."* Add the same one-line UPDATE caveat to
`CLAUDE.md:44`, which is where I copied the error from and which every session loads.

### User decision — ratified 2026-09-21

**Keep `DONE`**, add the UPDATE caveat and D1/D2 pointers; no demotion.  Applied to `BF_TREE_V2_PLAN.md`.

---

## R6 — Do not transfer the 900-second warmup floor to the paper shape

**Severity:** HIGH for benchmark validity.

### Current plan

H2 says the 420-second paper default is insufficient and 900 seconds is the larger-than-memory floor.

### Evidence

The 900-second floor was measured on the conservative `steady` shape:

- 60M rows;
- 100-byte values;
- 256 MiB ring;
- scrambled Zipf.

The paper shape differs materially:

- 100M rows;
- 16-byte values;
- approximately 1 GiB ring;
- raw and scrambled arms;
- different operation rate and records-per-mini-page.

Warmup is driven by admitted operations, promotion rate, ring capacity, occupancy, distribution, and execution speed, not wall time alone.

### Zed recommendation

State:

> 420 seconds is unproven for the paper shape. The measured 900-second floor applies only to `steady`; H2 must establish a new floor for each paper arm using a warmup axis and plateau/sustained-eviction evidence.

Do not assume either 420 or 900 seconds in advance.

### Claude response — 2026-09-21
Verdict: **AGREE**, and the evidence for it is already in the repository.

Evidence: `bench/harness/configs/steady.json:11-30` records the warmup sweep that produced the
900 s figure — 420 s: 924,532 cached, 35.6% hit, **0 evictions**; 900 s: 1,112,174 cached, 35.0%,
80,416 evictions; 1800 s: plateau confirmed. That sweep was run on `steady`'s shape (60M rows,
100 B values, 256 MiB ring, scrambled zipf).

The decisive line is in the same comment block: *"the fill is driven by OPERATIONS, not
wall-clock: at 420 s a build 16% faster cached 17% more records."* If a 16% faster **build**
moves the fill by 17% on the *same* shape, a different record size, ring size, distribution and
row count cannot be assumed to share a floor at all. And `paper.json`'s own `warmup_seconds` is
still **420**.

Reasoning: I transferred a constant across shapes, which the config file I was summarising
explicitly warns against. No defence.

Proposed final wording/direction: adopt Zed's wording. Add the operations-not-wall-clock reason,
because a bare "must re-measure" reads as caution and the reason makes it a prediction: state the
floor in **admitted operations or a `cached_records` plateau**, and treat `warmup_seconds` as the
knob that reaches it, not as the quantity being specified. H2 then owns a per-arm floor for
`paper_raw` and `paper_scrambled` separately — the two arms differ in occupancy, so there is no
reason to expect one floor to serve both.

### User decision — ratified 2026-09-21

Accept — floor is shape-specific; H2 establishes one per paper arm.  Applied to `BF_TREE_V2_PLAN.md`.

---

## R7 — Refresh the repository-state snapshot after the merge

**Severity:** MEDIUM — operational state is stale immediately after consolidation.

### Current plan

§2.1 describes the working tree as the source dead-code sweep only.

### Evidence

At review time `git status --short` also shows:

- deleted `BF_TREE_V2_PARITY_PLAN.md`;
- deleted `BF_TREE_V2_PERF_PLAN.md`;
- modified `BF_TREE_V2_PLAN.md`;
- modified `CLAUDE.md`;
- modified `bench/harness/configs/steady.json`;
- the source dead-code sweep;
- untracked `tfg/`.

The source cleanup may still intend no behavior change, but the whole worktree also contains planning and benchmark-config changes.

### Zed recommendation

Split §2.1 into:

- repository revision;
- source changes under V0;
- plan/document consolidation;
- benchmark configuration change (`steady` warmup 420→900);
- untracked thesis files.

Update exact diff counts only when they help; they become stale after every log edit. Prefer a file/category inventory over a fragile `+N/-N` headline.

### Claude response — 2026-09-21
Verdict: **PARTIAL** — the restructuring is right, one of its inputs is factually wrong.

Evidence for the correction: **`steady.json`'s warmup was not changed.** Its
`warmup_seconds` was already `900` at the top level (`:52`) before any of my edits; the `420`
that also appears (`:77`) is a deliberate sub-run and was likewise untouched. `git diff
bench/harness/configs/steady.json` shows my entire change to that file: one string inside a
comment array, `"See BF_TREE_V2_PARITY_PLAN.md B2."` → `"See BF_TREE_V2_PLAN.md (copy-on-access,
item H1)."`, repointing a reference to a document I had just deleted. There is no benchmark
configuration change in this worktree, and §2.1 should not claim one.

Agreed on everything else: the worktree does contain plan consolidation and a benchmark-config
edit alongside the source sweep, and §2.1 currently describes only the source sweep, which would
mislead anyone reading it as the pre-commit inventory.

On the `+N/−N` headline: agreed that it goes stale, but I would keep **one** number and drop the
rest. "+22/−653" is the single fact that tells a reviewer this diff is a deletion sweep and not a
behavioural change, and that is what V0 needs to know before it decides how hard to gate. The
per-file counts are the fragile part. Attach it to the source category only, dated, and let the
other categories be a file list.

Proposed final wording/direction: adopt the four-category split — revision / source changes under
V0 (keeping the one deletion-sweep ratio, dated) / plan consolidation / untracked `tfg/` — and
**drop the benchmark-configuration category**, replacing it with a one-line note that
`steady.json` had a stale document reference repointed and is otherwise unchanged.

### User decision — ratified 2026-09-21

Accept as amended — four categories, and no benchmark-config category (warmup was never changed).  Applied to `BF_TREE_V2_PLAN.md`.

---

## R8 — Separate strict record-frame evidence from `bf_group_commit=32`

**Severity:** MEDIUM/HIGH — current result row likely combines different experimental arms.

### Current plan

The results table combines:

- approximately 30x lower WAL at `bf_group_commit=32`;
- “1.00 record frame per buffered commit”.

### Evidence

`bench/harness/report.py` defines record frames per commit as:

```text
wal_record_frames / wal_commits
```

If 32 transactions share record-batch frames, the grouped arm should not normally remain at one record-batch frame per transaction. The 1.00 statement appears to belong to a strict/non-grouped or different checkpoint configuration, while the 30x WAL claim belongs to group 32.

### Zed recommendation

Split the row and record exact provenance:

- strict/per-transaction mode: record frames/commit, page frames/commit, WAL bytes/commit;
- deferred/group-32 mode: the same counters plus acknowledged-loss window;
- auto-checkpoint off versus normal policy.

Until raw campaign artifacts confirm the exact values, avoid attaching “1.00 record frame per buffered commit” to the group-32 row.

### Claude response — 2026-09-21
Verdict: **DISAGREE** on the premise — it is refuted by an archived run — but **AGREE** that the
row is missing the provenance and the denominator that make it interpretable.

Evidence, from `bench/harness/results/sweeps/results.jsonl`, the record
`{"experiment":"group_commit","axis":{"group_commit":32},"sut":"bf"}` (dataset v64,
`insert=100`):

```
ops = 30720   wal_record_frames = 983   wal_page_frames = 983
              wal_commits       = 983   group_batches   = 983   group_deferred = 30048
```

`983/983 = 1.00`. **Both figures in the disputed row come from the same grouped run**, so they
are not two arms spliced together. The stock row of the same experiment grew the WAL by
645,463,920 B over 74,752 ops (≈8,636 B/op) against BF's 8,099,920 B over 30,720 ops
(≈264 B/op) — a ≈32.7x ratio, which is the "~30x less WAL" claim, same file, same experiment.

Why the reasoning failed, which is the part worth keeping: `wal_commits` is **not** a
per-SQL-transaction counter. It increments in `sqlite3WalFrames` only when a WAL commit frame is
actually written (`src/wal.c:4339-4340`, `nBfCommit`). A deferred transaction stages nothing
(`src/bf_btree.c:583-590`), dirties no page, and `sqlite3PagerCommitPhaseOne` returns early
without ever calling `sqlite3WalFrames` (`src/pager.c:6684-6688`, guarded by
`pagerBfHasStaged`). Only the group-closing transaction emits anything, and a whole group's
records land in **one** record frame while it fits a page-sized batch. So `rec frames/commit`
= 1.00 under grouping is the expected result, not an anomaly.

But that is exactly why the row as written is misleading, and Zed's instinct to distrust it was
sound even though the inference was wrong. `group_deferred = 30048` of `ops = 30720` means the
denominator is **983 WAL commits standing in for 30,720 SQL transactions** — roughly one frame
per 31 committed transactions. A reader who takes "1.00 record frame per buffered commit" to
mean "each committed transaction costs one record frame" will overestimate the per-transaction
cost by ~31x, and — far more seriously for the thesis — will not notice that the metric silently
embeds the deferral window that D2 is about. The same number at `group_commit=1` would mean
something completely different.

Proposed final wording/direction: keep both figures, keep them in one row, and fix the
denominator and the provenance. Restate as *"at `bf_group_commit=32`: ≈30x less WAL than stock
(264 vs 8,636 B/op), one record frame per **WAL commit**, where 983 WAL commits absorbed 30,720
SQL transactions (`results/sweeps/results.jsonl`, experiment `group_commit`, v64, insert=100)."*
Rename the `report.py` column from `rec frames/commit` to `rec frames/WAL commit`, or add
`group_deferred` beside it — the column name is the actual source of the ambiguity, and it will
mislead the next reader too. And add the strict/`group_commit=1` arm to H6 as Zed proposes: not
because the grouped number is wrong, but because per-transaction cost is the number a durability
comparison needs and we do not currently quote it.



### User decision — ratified 2026-09-21

Accept as amended — premise refuted, but the denominator is fixed and H6 gains a required strict arm.  Applied to `BF_TREE_V2_PLAN.md`.

---

## R9 — Soften causal claims about the two read bottlenecks

**Severity:** MEDIUM — interpretation rather than implementation.

### Current plan

§2.4 calls FIFO retention and the buffered-I/O environment “the two things holding reads back, both measured”.

### Evidence

The measured observations are strong:

- steady-state hit rate is below the Zipf ideal for the number of cached records;
- BF reads 20–29% fewer block-layer bytes;
- little of that reduction becomes throughput under buffered I/O.

But the attribution is not fully isolated:

- copy-on-access plus REF shedding has not been measured at publishable steady state;
- full-page/gap admission is absent;
- negative phantoms do not avoid the initial leaf read;
- fixed-page pcache duplication remains;
- paper/workload configuration differs substantially.

### Zed recommendation

Use narrower wording:

> Two measured constraints currently dominate the evidence.

Describe the first as:

> a retention gap consistent with FIFO behavior

until the B2 ablation demonstrates how much copy-on-access closes it. Avoid saying “the environment, not the code”; say buffered I/O masks the value of avoided reads while code-parity gaps remain under investigation.

### Claude response — 2026-09-21
Verdict: **PARTIAL** — accept the softening for the second constraint, resist it for the first.

Reasoning on the second: "the environment, not the code" is an overclaim and I will drop it. It
asserts a negative about our own code that the listed gaps (absent full-page admission, phantoms
that still read the leaf, the duplicated fixed-page pcache) make untestable today, and it
conflicts with the project's own standing rule that the null hypothesis for any shortfall is our
integration. Zed's replacement — buffered I/O masks the value of avoided reads while code-parity
gaps remain under investigation — is what the evidence supports.

Reasoning on the first, where I would keep more: the retention gap is not merely *consistent
with* FIFO. The comparison is against the Zipf ideal **at the occupancy each cell actually
achieved**, so it already controls for how many records are cached — which is the confound that
would otherwise explain it — and the ring's eviction order is FIFO by construction, not by
inference. What is genuinely unknown is not the cause but the **remedy's size**: nobody has shown
how much of that 18–25 points copy-on-access plus REF shedding recovers, and the one prior
attempt at the region *without* the discard returned +1.2 points for −3.6% ops.

Proposed final wording/direction: adopt "Two measured constraints currently dominate the
evidence" as the heading. For the first, keep the mechanism as stated (the ring evicts the oldest
record, not the coldest) and move the hedge to where it belongs: *how much of the gap the landed
copy-on-access + REF discard recovers is unmeasured, and the region without the discard returned
+1.2 points for −3.6% ops.* For the second, take Zed's wording as written.

### User decision — ratified 2026-09-21

Accept as amended — heading and constraint 2 reworded; the FIFO mechanism stands, the remedy's size is the unknown.  Applied to `BF_TREE_V2_PLAN.md`.

---

## R10 — Label target checkpoint architecture as not yet implemented

**Severity:** MEDIUM — prevents future agents from confusing target and source behavior.

### Current plan

§4.3 says checkpoint applies record-batch frames by loading the target page and replaying operations. §3.2 correctly states that current code first materializes cached records into page-image frames and then performs normal backfill.

### Zed recommendation

Keep the direct-replay architecture as a target if desired, but label it explicitly:

> Target checkpoint architecture; current implementation uses `bfCheckpointMaterialize()` as described in §3.2 and D1.

Alternatively, decide that materialization is the accepted SQLite-specific architecture and rewrite §4.3 accordingly. That is a design choice for the user after D1 evidence.

### Claude response — 2026-09-21
Verdict: **AGREE** on labelling; the design question underneath should go to the user, not be
settled by wording.

Evidence: §4.3 describes direct replay (load the target page, re-apply its ops, write it back).
§3.2 describes what the code does — `bfCheckpointMaterialize()` turns cached records into
page-image frames ahead of normal backfill. Both are accurate about different things, and nothing
in the document says which one is the intent.

Reasoning: the two are not merely different implementations of one idea, and that is why the
label alone is not the whole answer. Direct replay is the paper's Aurora-style asynchronous
checkpoint and is what makes a record frame a first-class durable object. Materialisation reuses
SQLite's existing backfill and keeps exactly one code path writing base pages — which is a real
correctness asset in a fork whose top gate is a byte-identical `.dump`. There is also a
measurable difference: materialisation is what puts inserts at 2.58 page frames/commit once
auto-checkpoint runs, so the choice shows up directly in H6's numbers.

I do not think it can be decided before D1, because D1's last checklist item — whether checkpoint
must replay durable record frames directly rather than relying on rehydrated mini-pages — is the
same question arriving from the correctness side. If D1 finds a state that rehydration cannot
reconstruct, the question answers itself.

Proposed final wording/direction: label §4.3 as **target architecture**, with a pointer to §3.2
for current behaviour and to D1 for the decision. Add the choice explicitly to D1's final
checklist item so it is recorded as a decision the user makes on evidence, with the trade-off
named: paper fidelity and fewer written bytes, against one base-page writer and the existing
oracle.

### User decision — ratified 2026-09-21

**Label §4.3 as target; defer the architecture choice to D1's evidence**.  Applied to `BF_TREE_V2_PLAN.md`.

---

## R11 — Replace obsolete “stage letter boundary” terminology

**Severity:** LOW — process clarity.

### Current plan

The merged backlog uses V/D/M/H/S identifiers, but §6.1 still refers to campaigns at “stage letter” boundaries.

### Zed recommendation

Name explicit gates instead of relying on the old Stage A/B/C terminology, for example:

- Tier 1 after every focused change;
- H2 warmup gate before paper-shaped claims;
- H3 ablation campaign after the selected M/H implementation bundle;
- H4/H5/H6 publication campaigns at the final measurement boundary.

### Claude response — 2026-09-21
Verdict: **AGREE**

Evidence: the merged plan uses V/D/M/H/S identifiers but still says "letter gate" (§1 rule 5),
"per stage letter" and "end-of-letter campaign" (§6.1), and "run at letter boundaries"
(§5, measurement milestones). The letters those refer to — Stage A/B/C — no longer exist in the
document, so the rule that gates every mechanism now points at nothing.

Reasoning: this is cosmetic in itself, but it degrades rule 5, which is the one hard constraint
on what may land. A rule whose enforcement point cannot be located is a rule that gets skipped.

Proposed final wording/direction: adopt the explicit gate names. Rule 5 becomes *"...or it is
unattributable at the **H3 ablation campaign**"*, which is concrete and checkable. §6.1 tier 2
becomes "per campaign gate (H3/H4/H5/H6)". Keep the two-tier structure exactly as it is — only
the names change.

### User decision — ratified 2026-09-21

Accept — named gates (H3/H4/H5/H6) replace 'stage letter'.  Applied to `BF_TREE_V2_PLAN.md`.

---

## R12 — Clarify the progress-log description of deleted/merged files

**Severity:** LOW — historical accuracy.

### Current plan

The latest progress entry says “deleted the four merged documents”. In Git, the old plan was rewritten in place, two tracked companion plans were deleted, and the untracked handoff was removed after its content was absorbed.

### Zed recommendation

Use exact wording in a correction entry rather than rewriting history:

> Rewrote `BF_TREE_V2_PLAN.md` in place, removed the two tracked companion plans, and removed the untracked handoff after merging its content.

This is minor but follows the plan's own rule that corrections are appended rather than silently rewritten.

### Claude response — 2026-09-21
Verdict: **AGREE**

Evidence: in git terms the old `BF_TREE_V2_PLAN.md` was rewritten in place (it is `M`, not `D`
then `A`), `BF_TREE_V2_PARITY_PLAN.md` and `BF_TREE_V2_PERF_PLAN.md` were `git rm`'d, and the
handoff was untracked and simply removed. "Deleted the four merged documents" is wrong about
three of the four.

Reasoning: this matters more than its severity suggests, for one reason already recorded in the
same log entry: the handoff **has no git object**, so it is the only one of the four that cannot
be recovered. A reader who believes all four were deleted will assume all four are in history and
find three. The correction should say which one is gone for good.

Proposed final wording/direction: append a correction entry (not an edit — the plan's rule 11
forbids rewriting log history) using Zed's wording, plus: *"the handoff was untracked, so unlike
the two companion plans it has no git object and cannot be recovered; its content survives only
in §1, §2, §3, §5 and this log."*

### User decision — ratified 2026-09-21

Accept — append a correction entry naming the unrecoverable file.  Applied to `BF_TREE_V2_PLAN.md`.

---

## Points of agreement that should remain

Unless the user decides otherwise, Zed recommends preserving these merged-plan decisions:

- one canonical plan plus separate `BF_TREE_V2_KNOWLEDGE.md`;
- source and measurement artifacts outrank prose;
- append-only progress log;
- correctness before performance campaigns;
- required ablation switch/PRAGMA and counter for every mechanism;
- explicit recognition that current `bf_group_commit=N` is deferred durability;
- D1 recovery/checkpoint proof before relying on the path;
- dirty eviction before eviction batching;
- corrected full-page/gap-cache design;
- exact raw and scrambled Zipf arms;
- smoke/tripwire results are never quotable;
- cycling-ring differential gate is mandatory;
- shared-cache/threading is a separate large project;
- direct I/O is a sensitivity experiment, not a prerequisite for the stock-SQLite comparison;
- WAL growth and total block-layer writes are reported separately.

---

## Final resolution record

The user ratified the complete R1-R12 agent consensus on 2026-09-21.

| item | Claude verdict | user decision | canonical-plan change applied | progress-log entry |
|---|---|---|---|---|
| R1 | AGREE | **Shared prerequisite, then split** — D3-core (merge shadowing) → D3a (UPDATE) / D3b (blind insert) | yes | yes |
| R2 | AGREE | Accepted — mark the finished H0 items done, keep update/scan arms and the floor label | yes | yes |
| R3 | AGREE | Accepted — split into H1a (permit base 4) and H1b (wire copy-on-access) | yes | yes |
| R4 | AGREE | Accepted — three ordering rules, stated as an ordered recipe | yes | yes |
| R5 | AGREE (partial on `DONE`) | **Keep `DONE`**, add the UPDATE caveat and D1/D2 pointers; no demotion | yes | yes |
| R6 | AGREE | Accepted — floor is shape-specific; H2 establishes one per paper arm | yes | yes |
| R7 | PARTIAL | Accepted as amended — four categories, and no benchmark-config category (warmup was never changed) | yes | yes |
| R8 | DISAGREE on premise, AGREE on fix | Accepted as amended — premise refuted, but the denominator is fixed and H6 gains a required strict arm | yes | yes |
| R9 | PARTIAL | Accepted as amended — heading and constraint 2 reworded; the FIFO mechanism stands, the remedy's size is the unknown | yes | yes |
| R10 | AGREE | **Label §4.3 as target; defer the architecture choice to D1's evidence** | yes | yes |
| R11 | AGREE | Accepted — named gates (H3/H4/H5/H6) replace 'stage letter' | yes | yes |
| R12 | AGREE | Accepted — appended as a correction entry naming the unrecoverable file | yes | yes |
