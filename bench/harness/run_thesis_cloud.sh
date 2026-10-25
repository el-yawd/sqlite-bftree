#!/bin/sh
# The thesis read campaign on a fresh, dedicated machine (2026-10-24).
#
#   git clone <repo> && cd sqlite-bftree-v2 && git checkout thesis
#   sh bench/harness/run_thesis_cloud.sh                       # thesis_reads (v1, 115 runs, ~10 h)
#   sh bench/harness/run_thesis_cloud.sh run thesis_reads_v2   # v2: equal leaf memory, holdouts (~8.5 h)
#   sh bench/harness/run_thesis_cloud.sh check [config]        # preflight only
# Runs are SHUFFLED (runner --shuffle, grouped by dataset): without it every
# cell ran all stock repeats, then all bf repeats, so a slow window of the VM
# landed on one SUT -- a bias that repeats cannot remove (added 2026-10-25;
# v1 was started without it).
#
# Setup steps for a fresh machine (Amazon Linux 2023 / dnf, or Ubuntu): CLOUD.md.
# Needs: cc, make, tcl (for the amalgamation), python3 + numpy (cost model), systemd (the runner
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
CONFIG=${2:-thesis_reads}
[ -f "bench/harness/configs/$CONFIG.json" ] || { echo "no config $CONFIG" >&2; exit 1; }

command -v systemd-run >/dev/null || { echo "systemd-run missing" >&2; exit 1; }
[ -f /sys/fs/cgroup/cgroup.controllers ] || { echo "cgroup v2 required" >&2; exit 1; }
FREE_G=$(df -BG --output=avail bench/harness | tail -1 | tr -dc '0-9')
[ "${FREE_G:-0}" -ge 30 ] || { echo "only ${FREE_G}G free; need ~30G" >&2; exit 1; }
echo "git: $(git rev-parse --short HEAD) $(git rev-parse --abbrev-ref HEAD)$(git diff --quiet || echo ' DIRTY')"
echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2)  mem: $(free -g | awk '/Mem/{print $2}')G"
# Hardware counters: instructions/op is the campaign's second machine-
# independent metric.  Many VMs expose no PMU (on AWS, typically only
# full-socket or metal sizes do); the campaign still runs, but the report's
# instr/op column stays empty.  Probe it now rather than after 7 hours.
PROBE=$(mktemp -d)
cat > "$PROBE/p.c" <<'C'
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
int main(void){
  struct perf_event_attr a; memset(&a, 0, sizeof a);
  a.size = sizeof a; a.type = PERF_TYPE_HARDWARE;
  a.config = PERF_COUNT_HW_INSTRUCTIONS; a.exclude_kernel = 1; a.exclude_hv = 1;
  int fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
  if( fd<0 ){ perror("perf_event_open"); return 1; }
  return 0;
}
C
if cc -o "$PROBE/p" "$PROBE/p.c" 2>/dev/null && "$PROBE/p" 2>/dev/null; then
  echo "pmu: hardware counters available (instr/op will be reported)"
else
  echo "pmu: NOT available -- instr/op will be empty; device reads/op is unaffected." >&2
  echo "     (perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null))" >&2
fi
rm -rf "$PROBE"
python3 bench/harness/runner.py bench/harness/configs/$CONFIG.json --dry-run | tail -3
[ "$MODE" = check ] && exit 0

mkdir -p build
( cd build && { [ -f Makefile ] || ../configure --quiet; } && make sqlite3 >/dev/null )
sh bench/harness/build_suts.sh --all
OUT=bench/harness/results/$CONFIG
mkdir -p "$OUT"
python3 bench/harness/runner.py bench/harness/configs/$CONFIG.json \
    --continue-on-error --shuffle 1 2>&1 | tee "$OUT/campaign.log"
python3 bench/harness/report.py "$OUT"
# The cost model next to every point-read cell: predicted vs measured device
# reads/op (bench/harness/costmodel.py; needs numpy, ~1 min per dataset).
if python3 -c "import numpy" 2>/dev/null; then
  python3 bench/harness/costmodel.py --results "$OUT" > "$OUT/COSTMODEL.md"
  echo "cost model: $OUT/COSTMODEL.md"
else
  echo "numpy missing: skipped the cost model (run costmodel.py --results $OUT later)" >&2
fi
echo "done: $OUT/RESULTS.md"
