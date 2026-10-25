# Running the thesis campaign on a cloud machine

A step-by-step guide to setting up a cloud machine and running the thesis read
campaign (`configs/thesis_reads.json`, 115 runs, about 10 hours) on it.  Written
2026-10-24 for branch `thesis`.

The campaign measures point reads, scans and a read/scan mix: stock SQLite
against the BF build and its ablations.  It uses direct I/O and a 32 MiB budget,
plus 8M/128M budget sweeps.  Read `README.md` in this directory for the
methodology; this file is only how to get it running somewhere clean.

---

## 1. Pick the machine

What the benchmark needs, in order of importance:

1. **Local NVMe storage, not a network disk.**
   - Under direct I/O every cache miss is a real device read.
   - Network block storage (AWS EBS, GCP Persistent Disk, "volumes") adds
     0.3-1 ms of variable latency, plus IOPS caps and burst credits.
   - Device reads/op would still be correct, but throughput and latency would
     measure the network, not the cache.
2. **Dedicated CPU, not burstable or shared.**
   - Avoid AWS `t*` and `*-flex`, GCP `e2`/shared-core, and "shared vCPU" plans.
   - Running out of CPU credits mid-campaign ruins the comparison.
3. **Small is fine.**
   - The benchmark is single-threaded and every run is capped by cgroups.
   - 2 vCPU, 8 GiB RAM and ~30 GB of disk are enough.
4. **A standard Linux distribution with systemd and cgroup v2**: Ubuntu
   24.04 LTS or Debian 12.

Recommended (check current prices; these are rough on-demand figures):

| option | notes | rough cost |
|---|---|---|
| **AWS `m6id.large`** (2 vCPU, 8 GiB, 118 GB local NVMe) | the default choice | ~$0.12/h, ~$1.20 for the campaign |
| AWS `c6id.large` / `i4i.large` | same idea; `i4i` has steadier NVMe | ~$0.10-0.17/h |
| GCP `n2`/`c3` with one local SSD | if you have GCP credits | similar |
| Hetzner dedicated (AX line) | bare metal, no neighbours; good if you will re-run things all month | ~€40-60/month + setup |

Rules:
- **On-demand, never spot.**  The runner cannot resume a campaign that gets
  interrupted, and on-demand costs about a dollar more.
- **Every number you compare must come from the same instance type**, ideally
  from one campaign.  Never mix laptop and cloud numbers in one table.
- **Hardware counters (instructions/op) are often unavailable on small VMs.**
  On AWS usually only full-socket or `.metal` sizes expose the PMU.  The
  campaign runs fine without it and the `instr/op` column stays empty.  If you
  want instructions/op too, use a `.metal` instance or bare metal (Hetzner).
  Device reads/op, the primary metric, works everywhere.
- A free-tier box (e.g. `m7i-flex.large`, EBS only) is fine for **rehearsing**
  steps 2-6, not for the numbers you quote.

## 2. Launch and log in

- OS image: Ubuntu 24.04 LTS (x86_64).
- Root disk: 30 GB is plenty.  The data goes on the local NVMe, not the root disk.
- Security group or firewall: SSH only.

```bash
ssh ubuntu@<public-ip>
```

## 3. Install packages

```bash
sudo apt update
sudo apt install -y build-essential tcl python3 python3-numpy git zlib1g-dev tmux
```

`tcl` builds the SQLite amalgamation, and `python3-numpy` is for the cost model
(`costmodel.py`).

## 4. Format and mount the local NVMe

Instance-store NVMe is usually NOT mounted at boot.  Find it first: it is the
large disk with no partitions or mountpoint.  On `m6id`, `nvme0n1` is usually
the EBS root and `nvme1n1` the instance store, but **check `lsblk`, do not
assume.**

```bash
lsblk -o NAME,SIZE,TYPE,MOUNTPOINT,MODEL
# e.g. nvme1n1  110.6G disk            Amazon EC2 NVMe Instance Storage
sudo mkfs.ext4 -F /dev/nvme1n1            # ext4 or xfs; must support O_DIRECT (tmpfs does not)
sudo mkdir -p /data
sudo mount -o noatime /dev/nvme1n1 /data
sudo chown "$USER": /data
df -h /data
```

> **Instance-store data is erased when the instance stops or terminates.**
> A reboot keeps it; a stop does not.  Copy the results off (step 9) before
> you stop the machine.

## 5. Make `systemd-run --user` work over SSH

Every run is started as `systemd-run --user --scope -p MemoryMax=... -p
MemorySwapMax=0`, so the VM needs a user systemd manager that may set memory
limits.

```bash
sudo loginctl enable-linger "$USER"       # user manager stays up without a desktop session
# log out and back in once, then check:
systemd-run --user --scope -p MemoryMax=64M true && echo "systemd-run OK"
cat /sys/fs/cgroup/user.slice/user-$(id -u).slice/user@$(id -u).service/cgroup.controllers
# must list "memory"
```

If `memory` is missing, delegate it and log in again:

```bash
sudo mkdir -p /etc/systemd/system/user@.service.d
printf '[Service]\nDelegate=cpu cpuset io memory pids\n' | \
  sudo tee /etc/systemd/system/user@.service.d/delegate.conf
sudo systemctl daemon-reload
# log out, log in, re-run the two checks above
```

If `systemd-run` says "Failed to connect to bus", the user session has no
D-Bus.  Log out and in again after `enable-linger`, and check that
`echo $XDG_RUNTIME_DIR` prints `/run/user/<uid>`.

Optional: pin the CPU frequency governor.  The runner warns when it is not
`performance`.  On most cloud VMs there is no cpufreq interface at all, which
is fine.

```bash
sudo apt install -y linux-tools-common linux-tools-$(uname -r) 2>/dev/null
sudo cpupower frequency-set -g performance 2>/dev/null || echo "no cpufreq control (normal on VMs)"
```

Optional: allow hardware counters for your user, if the instance has a PMU.

```bash
cat /proc/sys/kernel/perf_event_paranoid   # 2 is enough (user-space counting)
```

## 6. Clone and preflight

Clone ONTO the NVMe: datasets (~9 GiB) and the per-run database copies live in
`bench/harness/work/` inside the checkout.

```bash
cd /data
git clone -b thesis https://github.com/el-yawd/sqlite-bftree.git   # or the SSH URL + a deploy key
cd sqlite-bftree
git log --oneline -1          # note the revision; the manifest records it too
sh bench/harness/run_thesis_cloud.sh check
```

The repository may be private.  Then use an SSH deploy key
(`ssh-keygen -t ed25519`, add the public key under the repo's
Settings → Deploy keys, read-only) or an HTTPS personal access token.

`check` prints:
- the git revision and whether the tree is dirty (it must be clean);
- the CPU and RAM;
- `pmu: hardware counters available` or `pmu: NOT available`;
- a dry run of the campaign's runs.

It runs nothing heavy.  It stops if `systemd-run` or cgroup v2 is missing, or
if there is less than 30 GB free.

## 7. Optional: a 15-minute smoke run

Before committing 10 hours, prove the whole path once with the small
regression config (12 runs, ~15 min):

```bash
cd /data/sqlite-bftree
mkdir -p build && (cd build && ../configure --quiet && make sqlite3)
sh bench/harness/build_suts.sh --pre 6a46013      # bf_pre = the 2026-10-08 read binaries, as in the local run
python3 bench/harness/runner.py bench/harness/configs/thesis_regress.json
python3 bench/harness/report.py bench/harness/results/thesis_regress
less bench/harness/results/thesis_regress/RESULTS.md
```

What to look at:
- every run says `0 failed`;
- the `dev reads/op` column is filled in, which means direct I/O works;
- `instr/op` is filled in, if the PMU is available.

The first run also generates the v100 dataset, which takes a few minutes.

## 8. Run the campaign

Use `tmux`, so the run survives an SSH disconnect:

```bash
cd /data/sqlite-bftree
tmux new -s camp 'sh bench/harness/run_thesis_cloud.sh; exec bash'
# detach: Ctrl-b then d       reattach later: tmux attach -t camp
```

The script, in order:
1. builds the amalgamation;
2. builds every SUT (`build_suts.sh --all`);
3. generates the datasets on first use, with the stock binary;
4. runs all 115 runs with `--continue-on-error`, so one bad run does not stop
   the night;
5. writes `RESULTS.md`, then `COSTMODEL.md` (model vs measured).

Progress, from another SSH session:

```bash
tail -f /data/sqlite-bftree/bench/harness/results/thesis_reads/campaign.log
grep -c '"returncode": 0' /data/sqlite-bftree/bench/harness/results/thesis_reads/results.jsonl
```

Do not run anything else heavy on the machine meanwhile: no builds, no
second campaign.

## 9. Copy the results off, then stop the machine

From your laptop:

```bash
rsync -av ubuntu@<public-ip>:/data/sqlite-bftree/bench/harness/results/thesis_reads/ \
      ~/Projects/tfg-stuff/sqlite-bftree-v2/bench/harness/results/thesis_reads_cloud/
```

What you get:
- `results.jsonl`: every run, raw;
- `manifest.json`: machine, kernel, git revision, compiler, amalgamation hash;
- `RESULTS.md`: the tables;
- `COSTMODEL.md`: predicted vs measured device reads/op;
- `campaign.log`.

Keep all of them: the thesis numbers must be traceable to `manifest.json`.

Then stop or terminate the instance in the console.  The instance-store disk
is erased at that point, which is fine once the results are copied.

## 10. What to record in the thesis

- The instance type, region, kernel, and the git revision from `manifest.json`.
- That storage was local NVMe under O_DIRECT (`dio.direct_io` = 1 in every
  run; `db_resident_pages` stays flat, which proves the OS cache served nothing).
- That device reads/op and, where available, instructions/op are the primary
  metrics, and throughput is secondary.
- The cost model's predictions next to the measurements (`COSTMODEL.md`, plus
  the plan's 2026-10-24 sweep predictions, written before the campaign).

## Troubleshooting

| symptom | cause / fix |
|---|---|
| `systemd-run not found, but the config asks for memmax caps` | not a systemd distro; use Ubuntu/Debian |
| `Failed to connect to bus` | no user session D-Bus: `loginctl enable-linger`, log out/in |
| runs die with OOM kills inside their scope | `memory` controller not delegated (step 5) |
| `dev reads/op` empty | direct I/O not active, e.g. the checkout is on tmpfs/overlay; clone onto the NVMe ext4/xfs mount |
| `instr/op` empty | the VM has no PMU (`check` says so); expected on most small instances |
| `only NG free; need ~30G` | the work dir needs ~30 GB; you cloned onto the root disk instead of `/data` |
| campaign stopped mid-way | `results.jsonl` keeps finished runs; re-run with `runner.py ... --only <experiment>` for the missing cells, on the SAME machine |
