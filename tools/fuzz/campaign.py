#!/usr/bin/env python3
"""Runs fuzz.py in chunks, one host slot per chunk (at most ten minutes each), until the requested guest time is reached.

  campaign.py --build BUILD_DIR --out OUT --minutes 32 [--chunk-seconds 540]

The slot lock is released between chunks so other users of the host get a turn. The result is a JSON summary on stdout.
"""
import argparse
import fcntl
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT = "/tmp/coal-testkit/slots/0"


def run_chunk(build, out, seconds, extra):
    held = os.environ.get("COAL_TESTKIT_SLOT_HELD")       # a test kit slot is already ours
    with open(SLOT, "a") as lock:
        if not held:
            fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            p = subprocess.run([sys.executable, os.path.join(HERE, "fuzz.py"), "--build", build, "--out", out,
                                "--seconds", str(seconds)] + extra, capture_output=True, text=True, timeout=seconds + 240)
        finally:
            if not held:
                fcntl.flock(lock, fcntl.LOCK_UN)
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--minutes", type=float, default=32)
    ap.add_argument("--chunk-seconds", type=int, default=540)
    a, extra = ap.parse_known_args()
    chunk = min(a.chunk_seconds, 540)
    spent = 0.0
    chunks = []
    while spent < a.minutes * 60:
        n = int(min(chunk, a.minutes * 60 - spent))
        t = time.monotonic()
        p = run_chunk(a.build, a.out, n, extra)
        spent += time.monotonic() - t
        new = [l for l in p.stdout.splitlines() if l.startswith("NEW FINDING")]
        chunks.append({"returncode": p.returncode, "new_findings": new})
        print("chunk %d: rc=%d new findings %d" % (len(chunks), p.returncode, len(new)), file=sys.stderr, flush=True)
        if p.returncode:
            print(p.stderr[-800:], file=sys.stderr)
            break
    stats = {}
    sp = os.path.join(a.out, "stats.json")
    if os.path.exists(sp):
        with open(sp) as f:
            stats = json.load(f)
    print(json.dumps({"chunks": chunks, "stats": stats,
                      "crashes": sorted(os.listdir(os.path.join(a.out, "crashes")))}, indent=1))
    sys.exit(1 if any(c["returncode"] for c in chunks) else 0)


if __name__ == "__main__":
    main()
