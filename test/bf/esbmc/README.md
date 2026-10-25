# ESBMC proofs: BF-Tree key encoding and WAL record codec

Bounded model checking (ESBMC 8.3.0) of the pieces of the fork where a bug
returns **wrong answers without crashing**, so no sanitizer or fuzzer
notices.  Precedent: in October 2026 the rowid encoding sorted negative
rowids after positive ones (`ORDER BY id` gave `-10,1,2,3,5,-4,-1`), and the
differential oracles missed it until a generator was changed.

```sh
sh test/bf/esbmc/run_all.sh        # all proofs + the copy check
```

## What is proven

| # | property | code under proof | scope | result |
|---|---|---|---|---|
| 1 | harness copies equal `src/` | `bfEncodeRowid`, `bfDecodeRowid` (`src/bf_btree.c`), `bfKeyCompare` (`src/bf_mini_page.c`) | token-for-token, comments and whitespace ignored | checked on every run |
| 2 | **rowid encoding preserves order and round-trips** | the three functions above | **every** pair of `i64` values | PROVEN (`--unwind 9`) |
| 3 | **WAL decoder is memory-safe and terminates** on any input | `sqlite3BfWalIterInit` / `IterNext`, real `src/bf_wal.c` | any 48-byte buffer content, any length 0..48 | PROVEN (`--unwind 50`) |
| 4 | WAL codec round-trips, one record per op kind (`-DOPFIX=0/1/3/4`) | `sqlite3BfWalBatchInit/Append/Finish` + iterator, real `src/bf_wal.c` | 64-byte batch; symbolic page numbers, key and value bytes, INSERT value length 0..8 | **NOT YET PROVEN** (parked 2026-10-24; the per-op split was written but never run) |
| 5 | WAL codec round-trips, two records in order (`-DROWOPS2`) | same | 64-byte batch; ops INSERT/DELETE; all fields symbolic | **NOT YET PROVEN** (parked) |

Property 2 is fully general: an `i64` has no other bound, and every loop is
unwound completely.

Precisely, property 2 means:
- `dec(enc(a)) == a`;
- `a < b` ⟺ `memcmp(enc(a), enc(b), 8) < 0`, and `a == b` ⟺ the bytes are equal;
- the inline big-endian compare the mini-page search uses agrees with `memcmp`.

Properties 3-5 are bounded by buffer size.
- Property 3 (decoder safety):
  - no out-of-bounds read, invalid pointer or overflow, from ESBMC's built-in
    checks;
  - every returned key and value lies inside the input;
  - the decoder returns DONE or CORRUPT within `n/11 + 1` calls.  Each record
    takes at least 11 bytes, so this proves it never loops without consuming
    input.
- Properties 4 and 5 (codec round-trip): every record the encoder accepts
  decodes with identical fields and bytes, in order, followed by DONE.  A
  record the encoder refuses (`BFWAL_FULL`) never reaches the decoder.

**Status, 2026-10-24: proving work parked by the owner.**  Properties 1-3
are proven and mutation-checked.  Properties 4-5 are written but unproven.  A
single run of 2 records x 4 symbolic ops at 96 bytes did not finish in 30
minutes at about 5.5 GB.  The smaller per-op configurations were never run.
Cite only 1-3.

## How the proofs were checked (and why that matters)

**Every run uses unwinding assertions.**  With `--no-unwinding-assertions`,
ESBMC silently drops the paths that need more loop iterations than
`--unwind`.  The first version of these harnesses, written by an agent and
reported as "proven", ran at `--unwind 2` over 8-iteration loops (and over a
1024-byte zero fill).  With assertions on, all three FAILED their unwinding
checks, so those "proofs" covered almost nothing.  The bounds above are large
enough for every loop, and ESBMC verifies that.

**Mutation checks** (2026-10-24) show that the proofs can fail:

| mutant | proof | outcome |
|---|---|---|
| encoding without the sign-bit flip (the real pre-2026-10-01 bug) | 2 | FAILED: "a>b => memcmp>0" |
| decoder value bound off by one (`nVal > remaining + 1`) | 3 | FAILED: "value inside the input" |

**Source asserts are proof obligations.**  `bf_shim.h` maps `assert(x)` to
`__ESBMC_assert`.  The first shim defined it as a no-op, which would have
silenced every invariant the code states.

## Limits (state these when citing)

- These are proofs of **components**, not of the engine.  The engine is
  covered by the differential, crash, power-loss and audit oracles
  (`bench/gate.py`).
- Properties 3-5 are bounded by buffer size, 48 / 64 bytes.  A real frame
  is up to a page.  The decoder's per-record logic is the same at any size,
  but the proof does not cover longer inputs.
- The harness for property 2 proves a copy of three static functions.  Step 1
  of `run_all.sh` fails if the copy drifts from `src/`.

## Files

- `bf_shim.h`: SQLite typedefs and allocator seams; maps asserts to ESBMC.
- `harness_rowid_encoding.c`: property 2.
- `harness_wal_decoder_safety.c`: property 3.
- `harness_wal_codec.c`: properties 4 (default) and 5 (`-DROWOPS2`).
- `run_all.sh`: runs everything, unwinding assertions on.
