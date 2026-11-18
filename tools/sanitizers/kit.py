#!/usr/bin/env python3
"""Entry points of the debug-kernel suites for tools/testkit.

  kit.py guests              every guest test program on the debug kernel, sanitizers in panic mode
  kit.py selftest            the 21 deliberate bugs, each must be reported
  kit.py fuzz [SECONDS]      fixed-seed fuzz run from the committed seed corpus; fails on a finding not in known_crashes.txt
  kit.py nightly [MINUTES]   a longer campaign in 9 minute chunks (takes the host slot itself, per chunk)

The debug build goes to $COAL_DEBUG_BUILD (default build/debug) and is rebuilt incrementally. Exit status 0 means pass.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PANIC = "coal.ubsan=panic coal.san=panic coal.lockdep=panic coal.kfence=100"
SEED = 20260101


def ensure_build():
    d = os.environ.get("COAL_DEBUG_BUILD") or os.path.join(REPO, "build", "debug")
    os.makedirs(d, exist_ok=True)
    for cmd in (["cmake", "-S", os.path.join(REPO, "src", "CoalOS"), "-B", d, "-DCOAL_PROFILE=test"],
                ["cmake", "--build", d, "-j", str(os.cpu_count() or 2)]):
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if p.returncode:
            print(p.stdout[-2000:])
            sys.exit("debug build failed")
    return d


def guests():
    sys.path.insert(0, HERE)
    import suite
    res = suite.run_suite(ensure_build(), cmdline=PANIC)
    for t in res["tests"]:
        print("%s %s (%.1f s)" % ("ok  " if t["ok"] else "FAIL", t["name"], t["seconds"]))
        if not t["ok"]:
            print(t.get("tail", ""))
    for line in res["kernel_reports"]:
        print("REPORT", line)
    if res.get("error"):
        print("ERROR", res["error"])
    return 0 if res["passed"] else 1


def selftest():
    p = subprocess.run([sys.executable, os.path.join(HERE, "selftest.py"), ensure_build()], capture_output=True, text=True)
    try:
        d = json.loads(p.stdout)
    except ValueError:
        print(p.stdout[-1500:], p.stderr[-1500:])
        return 1
    for c in d.get("cases", []):
        print("%s %s" % ("ok  " if c["ok"] else "FAIL", c["name"]))
    return 0 if d.get("passed") else 1


def fuzz(seconds):
    out = tempfile.mkdtemp(prefix="coalfuzz-")
    fz = os.path.join(REPO, "tools", "fuzz")
    return subprocess.call([sys.executable, os.path.join(fz, "fuzz.py"), "--build", ensure_build(), "--out", out,
                            "--seconds", str(seconds), "--seed", str(SEED), "--seed-corpus", os.path.join(fz, "corpus"),
                            "--known", os.path.join(fz, "known_crashes.txt")])


def nightly(minutes):
    fz = os.path.join(REPO, "tools", "fuzz")
    out = os.environ.get("COAL_FUZZ_OUT") or os.path.join(REPO, "build", "fuzz-nightly")
    return subprocess.call([sys.executable, os.path.join(fz, "campaign.py"), "--build", ensure_build(), "--out", out,
                            "--minutes", str(minutes), "--seed-corpus", os.path.join(fz, "corpus"),
                            "--known", os.path.join(fz, "known_crashes.txt")])


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    arg = float(sys.argv[2]) if len(sys.argv) > 2 else None
    if cmd == "guests":
        sys.exit(guests())
    if cmd == "selftest":
        sys.exit(selftest())
    if cmd == "fuzz":
        sys.exit(fuzz(int(arg or 300)))
    if cmd == "nightly":
        sys.exit(nightly(arg or 60))
    sys.exit(__doc__)
