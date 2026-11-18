#!/usr/bin/env python3
"""Regression test of the debug kernel's sanitizers: provoke each bug with `ksan_test` and check the report.

  flock /tmp/coal-testkit/slots/0 selftest.py BUILD_DIR      (BUILD_DIR configured with COAL_PROFILE=test)

Prints one JSON object {"passed", "cases": [{"op", "name", "ok", "detail"}]}; exit status 0 when every case passed.
Fatal cases (the report ends in a panic) each boot a fresh guest.
"""
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from coalguest import Guest, GuestDied  # noqa: E402

# op, arg, name, regex the kernel log must match, fatal
CASES = [
    (1, 0, "slab: write into the unused tail", r"KSAN: heap buffer overflow: .*bytes past the requested size corrupted at offset 24", False),
    (2, 0, "slab: write past the object", r"KSAN: heap buffer overflow: .*right red zone corrupted at byte 3", False),
    (3, 0, "slab: write before the object", r"KSAN: heap buffer underflow: .*left red zone corrupted at byte 14", False),
    (4, 0, "slab: double free", r"KSAN: heap double free", False),
    (5, 0, "slab: write after free", r"KSAN: heap use after free: .*written after free at offset 5", False),
    (6, 0, "slab: fresh memory is poisoned", r"ksan_test 6: 165", False),
    (7, 0, "slab: free of an interior pointer", r"KSAN: heap invalid free of .*not the start of a slab object", False),
    (8, 0, "pages: overflow of a large block", r"KSAN: heap buffer overflow: .*corrupted at offset 5008", False),
    (9, 0, "pages: write after free of a large block", r"KSAN: heap use after free: .*written after free at offset 100", False),
    (10, 40, "ubsan: shift", r"UBSAN: .*shift exponent 40 is too large for 32-bit type 'int'", False),
    (11, 2147483647, "ubsan: signed overflow", r"UBSAN: .*signed integer overflow: 2147483647 \+ 1", False),
    (12, 8, "ubsan: array index", r"UBSAN: .*index 8 is out of range", False),
    (13, 0, "lockdep: order inversion", r"LOCKDEP: lock order inversion", False),
    (14, 0, "lockdep: sleeping with a spinlock", r"LOCKDEP: mutex_lock may sleep with 1 spinlock", False),
    (16, 0, "kfence: out of bounds", r"KFENCE: (out-of-bounds write after the object|buffer overflow)", False),
    (17, 0, "kfence: use after free", r"KFENCE: use-after-free write", False),
    (18, 0, "kfence: overflow inside the page", r"KFENCE: buffer overflow \(canary after the object corrupted\)", False),
    (21, 0, "frames: write after free", r"KSAN: freed page .* was written after it was freed", False),
    (15, 0, "lockdep: recursive acquire", r"LOCKDEP: recursive acquire.*\n(.*\n)*.*KERNEL PANIC", True),
    (19, 0, "stack protector", r"stack smashing detected(.*\n)*.*KERNEL PANIC", True),
    (20, 0, "stack guard page", r"kernel stack overflow", True),
]


def run_case(build_dir, op, arg, name, pattern, fatal, cmdline):
    g = Guest(build_dir, cmdline=cmdline)
    try:
        g.boot()
        try:
            r = g.run("/usr/bin/ksan_test %d %d" % (op, arg), 30)
            out = "".join((r.get(k) or {}).get("text", "") for k in ("stdout", "stderr"))
        except GuestDied:
            out = ""
            if not fatal:
                return dict(op=op, name=name, ok=False, detail="guest died: " + g.tail(10))
        time.sleep(0.3 if fatal else 0)
        log = g.log() + "\n" + out
        ok = re.search(pattern, log) is not None
        if fatal and ok:
            ok = not g.alive() or "KERNEL PANIC" in log
        return dict(op=op, name=name, ok=ok, detail="" if ok else g.tail(25))
    finally:
        g.cleanup()


def main():
    build_dir = sys.argv[1]
    cmdline = " ".join(["coal.kfence=100"] + sys.argv[2:])
    only = {int(x) for x in os.environ.get("ONLY", "").split(",") if x}
    cases = [run_case(build_dir, *c, cmdline) for c in CASES if not only or c[0] in only]
    res = {"passed": all(c["ok"] for c in cases), "cases": cases}
    json.dump(res, sys.stdout, indent=1)
    print()
    return 0 if res["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
