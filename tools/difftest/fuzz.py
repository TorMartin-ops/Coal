#!/usr/bin/env python3
"""Generator mode: seeded random file/descriptor operation sequences run on the host and in Coal; each new
kind of divergence is shrunk to a minimal script.

    fuzz.py [--seeds N] [--start S] [--length L] [--max-cases K] [--ignore SIG ...] [--json FILE]

A divergence signature is (operation, host outcome, Coal outcome) where an outcome is an errno name or "ok".
Signatures already reported (or given with --ignore, e.g. "ftruncate:ok:ENOSYS") are skipped, so one run lists
several distinct problems instead of the same one many times. As a module: run(machine, ...) -> dict.
"""
import argparse
import json
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import difftest  # noqa: E402

PATHS = ["p0", "p1", "p2", "d0/p0", "d0/p1", "d1", "d0", "."]
FLAGS = ["r", "w", "rw", "rwc", "rwct", "rwca", "wc", "wcx", "rwcx", "wa", "rd", "w", "rwn"]
WORKDIR = "dtfz"


def gen(seed, length):
    rnd = random.Random(seed)
    s = lambda: rnd.randrange(4)
    p = lambda: rnd.choice(PATHS)
    off = lambda: rnd.choice([0, 1, 3, 4095, 4096, 5000, 70000, 100])
    ops = []
    for _ in range(length):
        k = rnd.choices(["open", "close", "write", "read", "pread", "pwrite", "lseek", "dup", "dup2", "ftruncate",
                         "truncate", "unlink", "mkdir", "rmdir", "rename", "link", "stat", "fstat", "readdir", "fsync",
                         "getfl", "access", "fcntl"],
                        [14, 5, 12, 10, 3, 3, 8, 3, 2, 4, 2, 5, 4, 3, 5, 2, 6, 4, 3, 1, 2, 2, 3])[0]
        if k == "open": ops.append("open %d %s %s" % (s(), p(), rnd.choice(FLAGS)))
        elif k in ("close", "fstat", "fsync", "getfl"): ops.append("%s %d" % (k, s()))
        elif k == "write": ops.append("write %d %d %s" % (s(), rnd.choice([1, 5, 100, 4096, 5000]), rnd.choice("abc")))
        elif k == "read": ops.append("read %d %d" % (s(), rnd.choice([1, 4, 100, 5000])))
        elif k == "pread": ops.append("pread %d %d %d" % (s(), rnd.choice([1, 4, 100]), off()))
        elif k == "pwrite": ops.append("pwrite %d %d %d %d" % (s(), rnd.choice([1, 7, 100]), off(), rnd.randrange(2)))
        elif k == "lseek": ops.append("lseek %d %d %d" % (s(), rnd.choice([0, 1, 5, -1, -3, 4096, 9000]), rnd.choice([0, 1, 2, 2, 0, 3])))
        elif k in ("dup", "dup2"): ops.append("%s %d %d" % (k, s(), s()))
        elif k == "ftruncate": ops.append("ftruncate %d %d" % (s(), rnd.choice([0, 1, 10, 4096, 9000, -1])))
        elif k == "truncate": ops.append("truncate %s %d" % (p(), rnd.choice([0, 3, 5000])))
        elif k in ("unlink", "mkdir", "rmdir", "stat", "readdir", "access"): ops.append("%s %s" % (k, p()))
        elif k in ("rename", "link"): ops.append("%s %s %s" % (k, p(), p()))
        elif k == "fcntl": ops.append("fcntl %d %d %d" % (s(), rnd.choice([1, 2, 3, 0]), rnd.choice([0, 1])))
    return ops


def outcome(res):
    return res if res.startswith("E") else "ok"


def op_of(label):
    return label.split(" ", 1)[0]


def divergences(host_out, coal_out):
    """[(index, op, signature)] for each differing line (compared by position: every script line yields one)."""
    h, g = host_out.splitlines(), coal_out.splitlines()
    out = []
    for i in range(max(len(h), len(g))):
        hl, gl = (h[i] if i < len(h) else None), (g[i] if i < len(g) else None)
        if hl == gl:
            continue
        ph, pg = difftest.parse(hl)[0] if hl else ("?", "missing", "", ""), difftest.parse(gl)[0] if gl else ("?", "missing", "", "")
        out.append((i, op_of(ph[0] if hl else pg[0]), "%s:%s:%s" % (op_of(ph[0] if hl else pg[0]), outcome(ph[1]), outcome(pg[1])), hl, gl))
    return out


class Runner:
    def __init__(self, machine, bin_dir):
        self.m = machine
        self.host_exe = os.path.join(bin_dir, "fsops")
        self.tmp = tempfile.mkdtemp(prefix="fuzz-")
        self.runs = 0
        self.m.run("mkdir -p %s" % difftest.GUEST_DIR)
        self.m.put(self.host_exe, "%s/fsops" % difftest.GUEST_DIR)

    def both(self, ops):
        self.runs += 1
        script = os.path.join(self.tmp, "s.txt")
        with open(script, "w") as f:
            f.write("\n".join(ops) + "\n")
        h = difftest.run_host(self.host_exe, args=[script, "/tmp/dt_" + WORKDIR + "_h"])
        self.m.put(script, "/tmp/dtfz_script")
        g = difftest.run_guest(self.m, "fsops", args=["/tmp/dtfz_script", "/tmp/dt_" + WORKDIR + "_h"])
        return h, g


def shrink(runner, ops, sig, known):
    """Delta debugging: smallest subsequence of ops whose first new divergence still has signature sig."""
    def bad(sub):
        h, g = runner.both(sub)
        return any(d[2] == sig for d in divergences(h["stdout"], g["stdout"]) if d[2] not in known - {sig})
    n = 2
    while len(ops) >= 2:
        chunk = max(1, len(ops) // n)
        reduced = False
        for i in range(0, len(ops), chunk):
            cand = ops[:i] + ops[i + chunk:]
            if cand and bad(cand):
                ops, n, reduced = cand, max(n - 1, 2), True
                break
        if not reduced:
            if chunk == 1:
                break
            n = min(len(ops), n * 2)
    return ops


def run(machine, seeds=40, start=1, length=40, max_cases=12, ignore=(), build_dir=None):
    bin_dir = build_dir or tempfile.mkdtemp(prefix="fuzzbin-")
    os.makedirs(bin_dir, exist_ok=True)
    exe = os.path.join(bin_dir, "fsops")
    subprocess.run(difftest.CC + ["-I" + difftest.CORPUS, "-o", exe, os.path.join(difftest.CORPUS, "fsops.c")], check=True)
    r = Runner(machine, bin_dir)
    known, cases, clean = set(ignore), [], 0
    for seed in range(start, start + seeds):
        ops = gen(seed, length)
        h, g = r.both(ops)
        new = [d for d in divergences(h["stdout"], g["stdout"]) if d[2] not in known]
        if g["signal"] or g["timed_out"]:
            new = new or [(len(g["stdout"].splitlines()), "crash", "crash:ok:CRASH", None, None)]
        if not new:
            clean += 1
            continue
        sig = new[0][2]
        small = shrink(r, ops, sig, known) if sig != "crash:ok:CRASH" else ops
        h2, g2 = r.both(small)
        d = next((x for x in divergences(h2["stdout"], g2["stdout"]) if x[2] == sig), None)
        cases.append({"seed": seed, "signature": sig, "script": small, "host": d[3] if d else None, "coal": d[4] if d else None})
        known.add(sig)
        print("seed %d: %s (%d ops)" % (seed, sig, len(small)), file=sys.stderr)
        if len(cases) >= max_cases:
            break
    return {"seeds_run": seed - start + 1, "clean_seeds": clean, "guest_runs": r.runs, "cases": cases}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seeds", type=int, default=40)
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--length", type=int, default=40)
    ap.add_argument("--max-cases", type=int, default=12)
    ap.add_argument("--ignore", nargs="*", default=[])
    ap.add_argument("--json")
    ap.add_argument("--build-dir", default=os.environ.get("COAL_BUILD_DIR", os.path.join(tempfile.gettempdir(), "difftest-coal")))
    a = ap.parse_args(argv)
    from machine import GatewayMachine
    m = GatewayMachine(a.build_dir)
    try:
        res = run(m, a.seeds, a.start, a.length, a.max_cases, a.ignore)
    finally:
        m.close()
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)
    for c in res["cases"]:
        print("\n%s  (seed %d)\n  host: %s\n  coal: %s\n  script:\n    %s" % (c["signature"], c["seed"], c["host"], c["coal"], "\n    ".join(c["script"])))


if __name__ == "__main__":
    main()
