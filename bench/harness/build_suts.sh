#!/bin/sh
# Build every system-under-test from ONE amalgamation.
#
# The BF code is inlined into sqlite3.c and whole-file guarded by macros, so a
# single generated sqlite3.c compiles into every variant just by changing -D
# flags.  That matters: it means "bf" and "stock" differ ONLY in those flags,
# not in compiler version, optimisation level, or amalgamation vintage.  Any
# other arrangement leaves a way for a measured difference to be an artefact.
#
# Variants:
#   stock         -DSQLITE_OMIT_BF_CACHE          upstream SQLite, no BF at all
#   bf            Phase 2 default                 read cache + write-back WAL
#   bf_off        BF compiled in, PRAGMA off      isolates fork overhead from
#                                                 the feature (run with
#                                                 --bf-cache off)
#   bf_ro         no SQLITE_BF_INSERT_BUFFERING   Phase 1: read cache only
#   bf_nomerge    -DSQLITE_BF_NO_MERGE_SCAN       ablations
#   bf_nowbdel    -DSQLITE_BF_NO_WRITEBACK_DELETE
#   bf_noshortcut -DSQLITE_BF_NO_DESCENT_SHORTCUT
#   bf_nocompact  -DSQLITE_BF_NO_MINIPAGE_COMPACT
#   bf_shed       -DSQLITE_BF_UPGRADE_SHED        opt-in: upgrades shed cold records
#   bf_nofull     -DSQLITE_BF_NO_FULL_PAGE        M2: no full-page (BF_LOC_FULL) copies
#
# Usage:
#   sh bench/harness/build_suts.sh            # core variants (stock, bf, bf_ro)
#   sh bench/harness/build_suts.sh --all      # + every ablation
#   sh bench/harness/build_suts.sh --profile  # + -g -fno-omit-frame-pointer
#   sh bench/harness/build_suts.sh --pre REV  # + bf_pre from another revision
#
# --pre REV builds an EXTRA SUT named "bf_pre" from a second amalgamation
# generated in a throw-away git worktree at REV (default HEAD), using this
# harness's bfbench.c and the identical COMMON flags.  That is the only way to
# A/B our own src/ changes: build_suts.sh otherwise builds every SUT from ONE
# amalgamation, which pins them all to the current working tree.  Cross-campaign
# throughput comparisons in this repo have spread up to 23%, so "before" has to
# be a SUT inside the same run, not an older results directory.
set -e
cd "$(dirname "$0")"
HARNESS=$(pwd)
ROOT=$(cd ../.. && pwd)
BUILD="$ROOT/build"
OUT="$HARNESS/bin"

ALL=0; PROFILE=""; PRE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --all) ALL=1 ;;
    --profile) PROFILE="-g -fno-omit-frame-pointer" ;;
    --pre) PRE=${2:-HEAD}; [ -n "${2:-}" ] && shift ;;
    --pre=*) PRE=${1#--pre=} ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

mkdir -p "$OUT"

# ---- 1. regenerate the amalgamation from src/ -------------------------------
echo "==> regenerating amalgamation"
( cd "$BUILD" && make -s sqlite3.c >/dev/null 2>&1 || make sqlite3.c )
[ -f "$BUILD/sqlite3.c" ] || { echo "no $BUILD/sqlite3.c" >&2; exit 1; }

# ---- 2. common flags --------------------------------------------------------
# Identical for every variant.  THREADSAFE=1 matches the project's normal build
# (the benchmark is single-threaded by design -- single-writer is a documented
# non-transfer -- but we do not want to measure a binary nobody else tests).
CC=${CC:-cc}
COMMON="-O2 $PROFILE -DSQLITE_THREADSAFE=1 -DSQLITE_DQS=0 \
 -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_DEFAULT_MEMSTATUS=1 \
 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
 -I$BUILD -I$ROOT/src"
LIBS="-lm -lpthread"

build_one() { # $1=name  $2=extra flags
  name=$1; extra=$2
  printf '  %-14s %s\n' "$name" "${extra:-<default>}"
  $CC $COMMON $extra -o "$OUT/bfbench_$name" \
      "$HARNESS/bfbench.c" "$BUILD/sqlite3.c" $LIBS
}

echo "==> compiling SUTs into $OUT"
build_one stock "-DSQLITE_OMIT_BF_CACHE"
build_one bf    "-DSQLITE_BF_INSERT_BUFFERING"
build_one bf_ro ""

if [ "$ALL" = 1 ]; then
  build_one bf_nomerge    "-DSQLITE_BF_INSERT_BUFFERING -DSQLITE_BF_NO_MERGE_SCAN"
  build_one bf_nowbdel    "-DSQLITE_BF_INSERT_BUFFERING -DSQLITE_BF_NO_WRITEBACK_DELETE"
  build_one bf_noshortcut "-DSQLITE_BF_INSERT_BUFFERING -DSQLITE_BF_NO_DESCENT_SHORTCUT"
  build_one bf_nocompact  "-DSQLITE_BF_INSERT_BUFFERING -DSQLITE_BF_NO_MINIPAGE_COMPACT"
  # Opt-IN, unlike the ablations above: size upgrades shed cold cache records
  # (the reference's copy-on-access cold discard, applied on the upgrade copy).
  build_one bf_shed       "-DSQLITE_BF_INSERT_BUFFERING -DSQLITE_BF_UPGRADE_SHED"
  # D3 ablations (2026-09-30): existing-row UPDATE buffering, blind REPLACE.
  build_one bf_noupd      "-DSQLITE_BF_INSERT_BUFFERING -DSQLITE_BF_NO_UPDATE_BUFFER"
  build_one bf_noblind    "-DSQLITE_BF_INSERT_BUFFERING -DSQLITE_BF_NO_BLIND_INSERT"
  # M2 ablation (2026-10-03): leaves are never copied whole into the ring.
  build_one bf_nofull     "-DSQLITE_BF_INSERT_BUFFERING -DSQLITE_BF_NO_FULL_PAGE"
fi

# bf_off is the same binary as bf; the runner passes --bf-cache off.
ln -sf "bfbench_bf" "$OUT/bfbench_bf_off"

# ---- 2b. bf_pre: the same driver against another revision's src/ ------------
PRE_REV=""
if [ -n "$PRE" ]; then
  PRE_REV=$(cd "$ROOT" && git rev-parse --short "$PRE")
  # OUTSIDE the repo on purpose: a nested worktree shows up as an untracked
  # directory, and full.json's "strict" gate fails on a dirty tree.
  WT="$ROOT/../.bf_pre_worktree_$(basename "$ROOT")"
  echo "==> building bf_pre from $PRE ($PRE_REV) in $WT"
  if [ -f "$WT/.bf_pre_rev" ] && [ "$(cat "$WT/.bf_pre_rev")" = "$PRE_REV" ]; then
    echo "    reusing existing worktree"
  else
    (cd "$ROOT" && git worktree remove --force "$WT" 2>/dev/null || rm -rf "$WT")
    (cd "$ROOT" && git worktree add --quiet --detach "$WT" "$PRE_REV")
    echo "$PRE_REV" > "$WT/.bf_pre_rev"
  fi
  mkdir -p "$WT/build"
  ( cd "$WT/build" || exit 1
    [ -f Makefile ] || ../configure --quiet
    make -s sqlite3.c >/dev/null 2>&1 || make sqlite3.c )
  [ -f "$WT/build/sqlite3.c" ] || { echo "no $WT/build/sqlite3.c" >&2; exit 1; }
  # Same driver, same flags, same optimisation level -- only src/ differs.
  # -I ordering mirrors COMMON: the pre tree's build/ and src/ come first.
  printf '  %-14s %s\n' bf_pre "src/ @ $PRE_REV"
  $CC -O2 $PROFILE -DSQLITE_THREADSAFE=1 -DSQLITE_DQS=0 \
      -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_DEFAULT_MEMSTATUS=1 \
      -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
      -I"$WT/build" -I"$WT/src" -DSQLITE_BF_INSERT_BUFFERING \
      -o "$OUT/bfbench_bf_pre" "$HARNESS/bfbench.c" "$WT/build/sqlite3.c" $LIBS
  PRE_SHA=$(sha256sum "$WT/build/sqlite3.c" | cut -d' ' -f1)
fi

# ---- 3. record exactly what was built ---------------------------------------
{
  echo "{"
  echo "  \"built_at\": \"$(date -Is)\","
  echo "  \"git_rev\": \"$(cd "$ROOT" && git rev-parse HEAD 2>/dev/null)\","
  # --untracked-files=no on purpose: "dirty" has to mean "the code that built
  # this binary is not the code at git_rev", and an untracked file cannot reach
  # the amalgamation without a TRACKED build input (main.mk, mksqlite3c.tcl)
  # also changing, which shows up here anyway.  Counting untracked files made
  # every build dirty as soon as an unrelated directory existed in the tree --
  # tfg/, the thesis -- which would have failed the strict gate on every future
  # campaign for a reason that has nothing to do with reproducibility.
  echo "  \"git_dirty\": $(cd "$ROOT" && [ -n "$(git status --porcelain --untracked-files=no)" ] && echo true || echo false),"
  echo "  \"sqlite_version\": \"$(cat "$ROOT/VERSION")\","
  echo "  \"cc\": \"$($CC --version | head -1)\","
  echo "  \"common_flags\": \"$(echo $COMMON)\","
  echo "  \"amalgamation_sha256\": \"$(sha256sum "$BUILD/sqlite3.c" | cut -d' ' -f1)\","
  echo "  \"bf_pre_rev\": \"${PRE_REV}\","
  echo "  \"bf_pre_amalgamation_sha256\": \"${PRE_SHA:-}\""
  echo "}"
} > "$OUT/build_manifest.json"

echo "==> done.  manifest: $OUT/build_manifest.json"
ls -1 "$OUT" | sed 's/^/     /'
