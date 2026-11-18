#!/usr/bin/env python3
"""Run the in-guest test programs on a build and report JSON, flagging sanitizer reports in the kernel log.

  flock /tmp/coal-testkit/slots/0 suite.py BUILD_DIR [--cmdline "coal.kfence=50"] [--only kernel_test,fs_stress]

Result (stdout, one JSON object): {"passed": bool, "tests": [{"name", "ok", "seconds", "exit_code", "tail"}],
"kernel_reports": [lines], "boot_seconds": n}. Exit status 0 when every test passed and the kernel log has no
report. Pluggable: `run_suite(build_dir, ...)` returns the same dict.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from coalguest import Guest, GuestDied, bug_lines  # noqa: E402

TESTS = [  # name, command, timeout, success marker (stdout) or None for exit status only
    ("kernel_test", "/usr/bin/kernel_test", 400, "0 failed"),
    ("fault_test", "/usr/bin/fault_test", 120, "0 failed"),
    ("syscall_test", "/usr/bin/syscall_test", 120, "0 failed"),
    ("libc_test", "/usr/bin/libc_test", 120, "0 failed"),
    ("fs_stress", "/usr/bin/fs_stress quick", 300, "0 failed"),
    ("applet_test", "/bin/sh /usr/bin/applet_test.sh", 200, None),
    ("sh_test", "/bin/sh /usr/bin/sh_test.sh", 200, None),
    ("memtest", "/usr/bin/memtest", 200, None),
]


def run_suite(build_dir, cmdline="", only=None, memory_mb=256, keep=False, logout=None):
    g = Guest(build_dir, cmdline=cmdline, memory_mb=memory_mb)
    res = {"build": build_dir, "cmdline": cmdline, "tests": [], "kernel_reports": [], "passed": False}
    try:
        g.boot()
        res["boot_seconds"] = round(g.boot_seconds, 1)
        for name, cmd, timeout, marker in TESTS:
            if only and name not in only:
                continue
            t0 = time.monotonic()
            entry = {"name": name}
            try:
                r = g.run(cmd, timeout)
                out = "".join((r.get(k) or {}).get("text", "") for k in ("stdout", "stderr"))
                ok = r.get("exit_code") == 0 and not r.get("timed_out") and (marker is None or marker in out)
                entry.update(ok=ok, exit_code=r.get("exit_code"), signal=r.get("signal"), tail=out[-600:])
            except GuestDied as e:
                entry.update(ok=False, exit_code=None, tail="guest died: %s" % e)
                res["tests"].append(entry)
                entry["seconds"] = round(time.monotonic() - t0, 1)
                break
            entry["seconds"] = round(time.monotonic() - t0, 1)
            res["tests"].append(entry)
        res["kernel_reports"] = bug_lines(g.log())
        res["kernel_log_tail"] = g.tail(40) if res["kernel_reports"] else ""
        res["passed"] = all(t["ok"] for t in res["tests"]) and not res["kernel_reports"] and bool(res["tests"])
    except GuestDied as e:
        res["error"] = str(e)
        res["kernel_reports"] = bug_lines(g.log())
    finally:
        if logout:
            with open(logout, "w") as f:
                f.write(g.log())
        (g.stop if keep else g.cleanup)()
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dir")
    ap.add_argument("--cmdline", default="coal.kfence=100")
    ap.add_argument("--only", default="")
    ap.add_argument("--memory", type=int, default=256)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--log", default=None, help="save the whole serial log here")
    a = ap.parse_args()
    res = run_suite(a.build_dir, a.cmdline, set(filter(None, a.only.split(","))) or None, a.memory, a.keep, a.log)
    json.dump(res, sys.stdout, indent=1)
    print()
    return 0 if res["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
