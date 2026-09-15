#!/bin/sh
# Unattended benchmark campaign.  Run it and go to bed.
#
#   sh bench/harness/run_overnight.sh              # full.json, then sweeps.json
#   sh bench/harness/run_overnight.sh full         # campaign only (~4-5 h)
#   sh bench/harness/run_overnight.sh sweeps       # ablations/tuning only (~1 h)
#
# What it takes care of, because each of these has silently ruined an
# unattended run before:
#
#   * SUSPEND.  This is a laptop.  A default Arch/CachyOS install suspends on
#     idle and on lid close, which kills every systemd scope mid-run and leaves
#     a half-written results.jsonl.  systemd-inhibit holds sleep/idle off for
#     the duration.  Close the lid only if you have also disabled
#     HandleLidSwitch; the inhibitor does not override an explicit lid event on
#     every configuration.
#   * ABORTING ON ONE BAD RUN.  --continue-on-error keeps going, records the
#     failure in results.jsonl, and lets the morning report tell you about it.
#     Without it a single OOM kill at 02:00 wastes the rest of the night.
#   * LOSING THE TERMINAL.  Everything is teed to a log under results/, so a
#     disconnected terminal costs you the live view, not the data.
#   * FORGETTING THE REPORT.  Reports are generated at the end of each config,
#     so RESULTS.md is waiting for you in the morning.
#
# Results land in bench/harness/results/<config>/{results.jsonl,manifest.json,RESULTS.md}
set -e
cd "$(dirname "$0")"
HARNESS=$(pwd)
WHICH=${1:-both}

CHECK=0
case "$WHICH" in
  full)   CONFIGS="full" ;;
  sweeps) CONFIGS="sweeps" ;;
  both)   CONFIGS="full sweeps" ;;
  smoke)  CONFIGS="smoke" ;;          # exercises this exact path in ~6 min
  check)  CONFIGS="full sweeps"; CHECK=1 ;;   # preflight only, runs nothing
  *) echo "usage: $0 [full|sweeps|both|smoke|check]" >&2; exit 2 ;;
esac

# Binaries must exist and be newer than the sources they were built from,
# otherwise the whole night measures stale code.
NEED="stock bf bf_ro bf_off bf_nomerge bf_nowbdel bf_noshortcut bf_nocompact"
STALE=0
for s in $NEED; do
  [ -e "bin/bfbench_$s" ] || STALE=1
done
[ -e bin/bfbench_bf ] && [ bfbench.c -nt bin/bfbench_bf ] && STALE=1
[ -e bin/bfbench_bf ] && [ ../../build/sqlite3.c -nt bin/bfbench_bf ] && STALE=1
if [ "$STALE" = 1 ]; then
  echo "==> SUTs missing or stale; rebuilding (this also refreshes the manifest)"
  sh ./build_suts.sh --all
fi

# Datasets total roughly 9 GiB, plus a ~7 GiB working copy and WAL headroom.
FREE_G=$(df -BG --output=avail . | tail -1 | tr -dc '0-9')
if [ "${FREE_G:-0}" -lt 30 ]; then
  echo "Only ${FREE_G}G free; the campaign wants ~20G plus headroom." >&2
  exit 1
fi

STAMP=$(date +%Y%m%d-%H%M)
run_one() {
  cfg=$1
  outdir="$HARNESS/results/$cfg"
  mkdir -p "$outdir"
  log="$outdir/run-$STAMP.log"
  echo "==> $cfg  (log: $log)"
  # --continue-on-error: one bad cell must not cost the rest of the night.
  python3 "$HARNESS/runner.py" "$HARNESS/configs/$cfg.json" \
      --continue-on-error 2>&1 | tee -a "$log" || true
  python3 "$HARNESS/report.py" "$outdir" 2>&1 | tee -a "$log" || true
}

main() {
  if [ "$CHECK" = 1 ]; then
    echo "==> preflight only; nothing will be run"
    for cfg in $CONFIGS; do
      python3 "$HARNESS/runner.py" "$HARNESS/configs/$cfg.json" --dry-run \
        | head -3
    done
    echo "==> binaries and disk are OK; re-run without 'check' to start"
    return 0
  fi
  echo "campaign started $(date -Is)"
  for cfg in $CONFIGS; do run_one "$cfg"; done
  echo "campaign finished $(date -Is)"
  for cfg in $CONFIGS; do
    echo "  -> $HARNESS/results/$cfg/RESULTS.md"
  done
}

# Hold off sleep/idle for the whole campaign, by re-executing ourselves once
# under the inhibitor.  Simpler and more portable than trying to ship the shell
# functions across the systemd-inhibit boundary.
if [ -z "$BFBENCH_INHIBITED" ] && command -v systemd-inhibit >/dev/null 2>&1; then
  BFBENCH_INHIBITED=1
  export BFBENCH_INHIBITED
  exec systemd-inhibit --what=sleep:idle --who="bfbench" \
       --why="BF-Tree benchmark campaign" \
       /bin/sh "$HARNESS/run_overnight.sh" "$WHICH"
fi
[ -n "$BFBENCH_INHIBITED" ] || echo "WARNING: no systemd-inhibit; machine may suspend mid-run." >&2

main
