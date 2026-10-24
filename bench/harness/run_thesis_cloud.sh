#!/bin/sh
# The thesis read campaign on a fresh, dedicated machine (2026-10-24).
#
#   git clone <repo> && cd sqlite-bftree-v2 && git checkout thesis
#   sh bench/harness/run_thesis_cloud.sh            # build, campaign, report (~7 h at 3 repeats)
#   sh bench/harness/run_thesis_cloud.sh check      # preflight only
#
# Needs: cc, make, tcl (for the amalgamation), python3, systemd (the runner
# isolates every run in a cgroup with systemd-run --user --scope), cgroup v2,
# ~30 GiB free disk on a filesystem that supports O_DIRECT (ext4/xfs; NOT tmpfs).
# Datasets (~9 GiB) are generated on first use by the stock binary.
#
# Provenance: the manifest records the git rev, compiler, amalgamation hash and
# machine; quote results only with it.  Device reads/op is the primary metric.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
MODE=${1:-run}

command -v systemd-run >/dev/null || { echo "systemd-run missing" >&2; exit 1; }
[ -f /sys/fs/cgroup/cgroup.controllers ] || { echo "cgroup v2 required" >&2; exit 1; }
FREE_G=$(df -BG --output=avail bench/harness | tail -1 | tr -dc '0-9')
[ "${FREE_G:-0}" -ge 30 ] || { echo "only ${FREE_G}G free; need ~30G" >&2; exit 1; }
echo "git: $(git rev-parse --short HEAD) $(git rev-parse --abbrev-ref HEAD)$(git diff --quiet || echo ' DIRTY')"
echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2)  mem: $(free -g | awk '/Mem/{print $2}')G"
python3 bench/harness/runner.py bench/harness/configs/thesis_reads.json --dry-run | tail -3
[ "$MODE" = check ] && exit 0

mkdir -p build
( cd build && { [ -f Makefile ] || ../configure --quiet; } && make sqlite3 >/dev/null )
sh bench/harness/build_suts.sh --all
OUT=bench/harness/results/thesis_reads
mkdir -p "$OUT"
python3 bench/harness/runner.py bench/harness/configs/thesis_reads.json \
    --continue-on-error 2>&1 | tee "$OUT/campaign.log"
python3 bench/harness/report.py "$OUT"
echo "done: $OUT/RESULTS.md"
