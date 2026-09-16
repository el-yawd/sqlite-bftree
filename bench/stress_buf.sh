#!/bin/sh
# Differential stress for the INSERT-BUFFERING build (Phase 2 merge-scan oracle).
# Compares ../build/sqlite3_buf (BF fork, -DSQLITE_BF_INSERT_BUFFERING) against
# ../build/sqlite3_stock (-DSQLITE_OMIT_BF_CACHE).  Buffered dirty records only
# exist in this build, so this is the oracle that exercises merge-iteration.
#
# Build sqlite3_buf from the fresh amalgamation after editing src/:
#   cd build && make -j sqlite3 && \
#   cc -fPIC -O2 -DSQLITE_BF_INSERT_BUFFERING -DSQLITE_ENABLE_MATH_FUNCTIONS \
#     -DSQLITE_THREADSAFE=1 -D_HAVE_SQLITE_CONFIG_H -DBUILD_sqlite -I. \
#     -I../src -DSQLITE_DQS=0 -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE \
#     -o sqlite3_buf shell.c sqlite3.c -lm -lz
#
# Usage: sh bench/stress_buf.sh [seeds...]   NOPS=8000 sh bench/stress_buf.sh
set -e
cd "$(dirname "$0")"
BUF=../build/sqlite3_buf
STOCK=../build/sqlite3_stock
NOPS="${NOPS:-4000}"
WORK=./stress_work
for bin in "$BUF" "$STOCK"; do
  [ -x "$bin" ] || { echo "MISSING: $bin" >&2; exit 2; }
done
mkdir -p "$WORK"; rm -f "$WORK"/buf_*
SEEDS="$*"
[ -z "$SEEDS" ] && SEEDS="1 2 3 5 7 11 13 17 23 42 99 123 777 1024 2024 12345 31337 65535"
fail=0; n=0
for seed in $SEEDS; do
  # Four generators: monotonic (gen_stress), random-rowid (gen_stress_rand,
  # mid-leaf buffering + splits), gen_merge_stress (full-table forward scans
  # INSIDE transactions with pending buffered inserts — the Stage 2.2 merge path
  # that the first two never reach, since their in-txn SELECTs are point/range),
  # and gen_rev_stress (DESC scans, LIMIT-ed reverse scans, count(*)/min/max in
  # the same state — the Stage 2.4 reverse-merge and merged-COUNT paths).
  for gen in gen_stress gen_stress_rand gen_merge_stress gen_rev_stress; do
    for jm in delete wal memory; do
      n=$((n+1))
      b="$WORK/buf_${gen}_s${seed}_${jm}"
      if [ "$gen" = gen_stress ]; then
        python3 ./gen_stress.py "$seed" "$jm" "$NOPS" > "$b.sql"
      elif [ "$gen" = gen_stress_rand ]; then
        # gen_stress_rand takes (seed, nops); prepend the journal pragma.
        printf 'PRAGMA journal_mode=%s;\n' "$jm" > "$b.sql"
        python3 ./gen_stress_rand.py "$seed" "$NOPS" >> "$b.sql"
      elif [ "$gen" = gen_merge_stress ]; then
        # gen_merge_stress takes (seed, n_txns); prepend the journal pragma.
        printf 'PRAGMA journal_mode=%s;\n' "$jm" > "$b.sql"
        python3 ./gen_merge_stress.py "$seed" 400 >> "$b.sql"
      else
        # gen_rev_stress takes (seed, n_txns); prepend the journal pragma.
        printf 'PRAGMA journal_mode=%s;\n' "$jm" > "$b.sql"
        python3 ./gen_rev_stress.py "$seed" 400 >> "$b.sql"
      fi
      # Optional BF knobs (BF_GROUP=N / BF_PROMOTION=N / BF_CACHE_SIZE=N).
      # Stock ignores the unknown pragmas, so both sides run the same script.
      #
      # These MUST land AFTER the generator's own PRAGMA block: changing
      # journal_mode reopens the pager and drops any BF setting made before it,
      # silently -- the pragma still reads back the value you set.  So insert
      # them just after the leading run of PRAGMA lines, not at the top.
      if [ -n "$BF_GROUP" ] || [ -n "$BF_PROMOTION" ] || [ -n "$BF_CACHE_SIZE" ]; then
        : > "$b.bf.pragmas"
        [ -n "$BF_GROUP" ] && \
          printf 'PRAGMA bf_group_commit=%s;\n' "$BF_GROUP" >> "$b.bf.pragmas"
        [ -n "$BF_PROMOTION" ] && \
          printf 'PRAGMA bf_promotion_rate=%s;\n' "$BF_PROMOTION" >> "$b.bf.pragmas"
        # BF_CACHE_SIZE=262144 makes the record ring small enough that these
        # workloads CYCLE it: eviction runs, blocks go on and come off the free
        # lists, and mini-pages are upgraded under memory pressure.  None of that
        # is reachable at the default size -- every benchmark in this repo
        # reports evictions=0 and the oracle databases are far smaller still --
        # which is how a segfault in bfFreeListRemove (bench/ring_repro.sh) lived
        # in committed code: the free-list chain is threaded through the freed
        # blocks, and nothing removed a block when the eviction head swept past
        # it.  Run a gate variant with this set.
        [ -n "$BF_CACHE_SIZE" ] && \
          printf 'PRAGMA bf_cache_size=%s;\n' "$BF_CACHE_SIZE" >> "$b.bf.pragmas"
        awk -v pf="$b.bf.pragmas" '
          !ins && !/^[Pp][Rr][Aa][Gg][Mm][Aa] / {
            while ((getline line < pf) > 0) print line
            close(pf); ins = 1
          }
          { print }
          END { if (!ins) { while ((getline line < pf) > 0) print line } }
        ' "$b.sql" > "$b.sql.tmp"
        mv "$b.sql.tmp" "$b.sql"
        rm -f "$b.bf.pragmas"
      fi
      rm -f "$b.bf.db" "$b.st.db"
      "$BUF"   "$b.bf.db" < "$b.sql" > "$b.bf.out" 2>&1 || true
      "$STOCK" "$b.st.db" < "$b.sql" > "$b.st.out" 2>&1 || true
      "$BUF"   "$b.bf.db" ".dump" > "$b.bf.dump" 2>&1 || true
      "$STOCK" "$b.st.db" ".dump" > "$b.st.dump" 2>&1 || true
      if ! diff -q "$b.st.out" "$b.bf.out" >/dev/null || \
         ! diff -q "$b.st.dump" "$b.bf.dump" >/dev/null; then
        fail=$((fail + 1))
        echo "FAIL  gen=$gen seed=$seed journal=$jm"
        diff "$b.st.out" "$b.bf.out" | head -12 || true
        diff "$b.st.dump" "$b.bf.dump" | head -12 || true
      else
        echo "PASS  gen=$gen seed=$seed journal=$jm"
      fi
    done
  done
done
echo ""
[ "$fail" = 0 ] && echo "ALL CLEAN: $n buffering differential tests passed." \
  || { echo "DIVERGENCES: $fail of $n FAILED."; exit 1; }
