#!/usr/bin/env python3
"""Re-run one campaign cell under perf and produce a flamegraph + self-time table.

Takes the EXACT bfbench argv a campaign recorded (results.jsonl), so the profile
is of the configuration that was measured, not of a hand-typed approximation.
Swaps in a frame-pointer build of the same SUT and a private copy of the
dataset, shortens the measured phase, and records with `perf record --call-graph
fp`.  Warmup is recorded too -- it is the same workload; pass --warmup-ops to
shorten it.

  python3 bench/tools/profile_arm.py RESULTS.jsonl INDEX BINARY OUTDIR \
      [--seconds 20] [--warmup-ops N] [--set FLAG=VALUE ...]

  INDEX   0-based line of results.jsonl to replay
  BINARY  frame-pointer bfbench build (build_suts.sh --profile, or by hand)
  --set   override/add a bfbench flag, e.g. --set --evict-target=0

Writes OUTDIR/{perf.data, flame.svg, self.txt, callers.txt, run.json}.
kernel.perf_event_paranoid=2 here: user-space samples only.
"""
import argparse, json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results"); ap.add_argument("index", type=int)
    ap.add_argument("binary"); ap.add_argument("outdir")
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--warmup-ops", type=int, default=None)
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--freq", type=int, default=999)
    a = ap.parse_args()

    rec = [json.loads(l) for l in open(a.results)][a.index]
    argv = list(rec["argv"])
    os.makedirs(a.outdir, exist_ok=True)

    def setflag(flag, val):
        if flag in argv:
            i = argv.index(flag)
            if val is None:
                del argv[i:i + 2]
            else:
                argv[i + 1] = val
        elif val is not None:
            argv.extend([flag, val])

    # Private db copy next to the original (same filesystem: direct I/O needs
    # one that honours O_DIRECT, which tmpfs does not).
    db = argv[argv.index("--db") + 1]
    src = os.path.join(os.path.dirname(db), "datasets", rec["dataset"] + ".db")
    pdb = os.path.join(os.path.dirname(db), "prof.db")
    for ext in ("", "-wal", "-shm"):
        if os.path.exists(pdb + ext):
            os.remove(pdb + ext)
    shutil.copyfile(src, pdb)
    argv[0] = os.path.abspath(a.binary)
    setflag("--db", pdb)
    setflag("--seconds", str(a.seconds))
    setflag("--json", os.path.join(a.outdir, "run.json"))
    if a.warmup_ops is not None:
        setflag("--warmup-ops", str(a.warmup_ops))
    for s in a.set:
        k, _, v = s.partition("=")
        setflag(k, v)

    data = os.path.join(a.outdir, "perf.data")
    cmd = ["perf", "record", "-q", "-F", str(a.freq), "--call-graph", "fp",
           "-o", data, "--"] + argv
    print(" ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)

    title = "%s %s %s" % (rec["experiment"], rec["sut"],
                          " ".join(s for s in a.set))
    script = subprocess.run(["perf", "script", "-i", data], check=True,
                            capture_output=True, text=True).stdout
    fg = subprocess.run([sys.executable, os.path.join(HERE, "flamegraph.py"),
                         os.path.join(a.outdir, "flame.svg"), title],
                        input=script, capture_output=True, text=True)
    open(os.path.join(a.outdir, "self.txt"), "w").write(fg.stdout + fg.stderr)
    rep = subprocess.run(["perf", "report", "-i", data, "--stdio", "--no-children",
                          "-g", "caller,0.5,callee", "--percent-limit", "1.5"],
                         capture_output=True, text=True).stdout
    open(os.path.join(a.outdir, "callers.txt"), "w").write(rep)
    for ext in ("", "-wal", "-shm"):
        if os.path.exists(pdb + ext):
            os.remove(pdb + ext)
    print("->", a.outdir)


if __name__ == "__main__":
    main()
