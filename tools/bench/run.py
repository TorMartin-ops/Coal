#!/usr/bin/env python3
"""Benchmark Coal OS: boot time, kernel size and the coalbench micro-benchmarks.

    run.py BUILD_DIR [--baseline results/baseline.json] [--out NAME]
    run.py --compare results/new.json --baseline results/baseline.json

BUILD_DIR is a CMake build directory; it is wiped and rebuilt first so the numbers
belong to the checked-out sources. Coal is driven through tools/coal-mcp/server.py
over MCP/stdio, exactly as Claude drives it. Results go to results/<git-sha>.json.
Standard library only (Python 3.9+).
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "harness"))
from mcpclient import McpClient  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
RESULTS = HERE / "results"
QEMU_ARGS = "-accel tcg,thread=single"      # one host thread runs the guest: less scheduling noise


class Mcp(McpClient):
    def __init__(self, build_dir, memory_mb, qemu_args):
        super().__init__(build_dir, memory_mb, {"COAL_QEMU_ARGS": qemu_args}, "coal-bench")


def percentile(values, p):
    s = sorted(values)
    k = p * (len(s) - 1)
    i = int(k)
    return s[i] + (s[min(i + 1, len(s) - 1)] - s[i]) * (k - i)


def spread(values):
    return {"median": statistics.median(values), "p10": percentile(values, 0.1),
            "p90": percentile(values, 0.9), "n": len(values), "samples": values}


def sh(*cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True).stdout.strip()


def kernel_size(build_dir):
    """text/data/bss of kernel.bin as `size` reports them, and its size when stripped."""
    kernel = Path(build_dir) / "kernel.bin"
    fields = sh("size", str(kernel)).splitlines()[-1].split()
    text, data, bss = (int(x) for x in fields[:3])
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "kernel.stripped"
        subprocess.run(["strip", "-s", "-o", str(out), str(kernel)], check=True)
        stripped = out.stat().st_size
    return {"text": text, "data": data, "bss": bss, "stripped": stripped}


def measure_boot(mcp, boots):
    """QEMU start to agentd ready. The server reports it in 0.1 s steps, so the wall time of
    coal_boot minus the (no-op) kernel rebuild it does first is used instead."""
    noop = []
    for _ in range(3):
        t = time.monotonic()
        mcp.tool("coal_build", target="iso")
        noop.append(time.monotonic() - t)
    overhead = statistics.median(noop)
    ms = []
    for _ in range(boots):
        t = time.monotonic()
        mcp.tool("coal_boot", timeout_s=120)
        ms.append((time.monotonic() - t - overhead) * 1000)
        mcp.tool("coal_shutdown")
    return spread(ms)


def run_coalbench(mcp, reps, runs):
    mcp.tool("coal_boot", timeout_s=120)
    per_run = []
    for _ in range(runs):
        text = mcp.tool("coal_run", timeout=1500, command="/usr/bin/coalbench -j -n %d" % reps, timeout_ms=600000)
        m = re.search(r"--- stdout ---\n(.*?)(\n--- stderr ---|\Z)", text, re.S)
        if not m or "exit code: 0" not in text:
            raise RuntimeError("coalbench failed:\n" + text)
        per_run.append(json.loads(m.group(1)))
    mcp.tool("coal_shutdown")
    tests = {}
    for t in per_run[0]["tests"]:
        same = [r["value"] for run in per_run for r in run["tests"] if r["name"] == t["name"] and r["value"] is not None]
        if not same:
            tests[t["name"]] = {"unit": t["unit"], "value": None}
            continue
        rows = [r for run in per_run for r in run["tests"] if r["name"] == t["name"] and r["value"] is not None]
        tests[t["name"]] = {"unit": t["unit"], "value": statistics.median(same),
                            "p10": statistics.median(r["p10"] for r in rows),
                            "p90": statistics.median(r["p90"] for r in rows),
                            "n": rows[0]["n"], "runs": len(rows),
                            "run_medians": same}
    return {"reps": per_run[0]["reps"], "tsc_mhz": per_run[0]["tsc_mhz"], "tests": tests}


def environment(qemu_args, memory_mb):
    cpu = ""
    try:
        cpu = next(l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name"))
    except (OSError, StopIteration):
        pass
    return {"host_cpu": cpu, "host_cpus": os.cpu_count(), "loadavg": os.getloadavg(),
            "qemu": sh("qemu-system-i386", "--version").splitlines()[0],
            "qemu_args": qemu_args, "guest_memory_mb": memory_mb, "python": sys.version.split()[0]}


def git_info():
    sha = sh("git", "rev-parse", "--short=12", "HEAD", cwd=REPO)
    dirty = bool(sh("git", "status", "--porcelain", "--untracked-files=no", cwd=REPO))
    return {"sha": sha, "dirty": dirty, "subject": sh("git", "log", "-1", "--format=%s", cwd=REPO)}


# --- comparison -------------------------------------------------------------------

def metrics(res):
    """Flat {name: (value, unit, lower_is_better, p10, p90)} view of one result file."""
    m = {}
    if res.get("boot"):
        b = res["boot"]
        m["boot_time"] = (b["median"], "ms", True, b["p10"], b["p90"])
    for k, v in (res.get("kernel") or {}).items():
        m["kernel_" + k] = (v, "bytes", True, v, v)
    for name, t in res["bench"]["tests"].items():
        m[name] = (t["value"], t["unit"], t["unit"] == "ns", t.get("p10"), t.get("p90"))
    return m


def compare(new, base):
    """Markdown table; '~' marks a change inside the p10..p90 band of both runs."""
    a, b = metrics(base), metrics(new)
    rows = [("metric", "unit", "baseline", "new", "change", "")]
    for name in list(b) + [n for n in a if n not in b]:
        if name not in a or name not in b or a[name][0] is None or b[name][0] is None:
            rows.append((name, (a.get(name) or b[name])[1], fmt(a[name][0]) if name in a else "n/a",
                         fmt(b[name][0]) if name in b else "n/a", "", ""))
            continue
        (v0, unit, lower, lo0, hi0), (v1, _, _, lo1, hi1) = a[name], b[name]
        pct = (v1 - v0) / v0 * 100 if v0 else 0.0
        noise = lo0 <= hi1 and lo1 <= hi0 and v0 != v1 and (hi0 > lo0 or hi1 > lo1)
        better = (v1 < v0) == lower
        verdict = "" if v0 == v1 else "~" if noise else "better" if better else "WORSE"
        rows.append((name, unit, fmt(v0), fmt(v1), "%+.1f%%" % pct, verdict))
    return table(rows)


def values(res):
    rows = [("metric", "unit", "median", "p10", "p90")]
    rows += [(n, m[1], fmt(m[0]), fmt(m[3]), fmt(m[4])) for n, m in metrics(res).items()]
    return table(rows)


def fmt(v):
    if v is None:
        return "n/a"
    return "%d" % v if float(v).is_integer() and abs(v) >= 1000 else "%.2f" % v


def table(rows):
    w = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    line = lambda r: "| " + " | ".join(c.rjust(w[i]) if i in (2, 3, 4) else c.ljust(w[i]) for i, c in enumerate(r)) + " |"
    return "\n".join([line(rows[0]), "|" + "|".join("-" * (x + 2) for x in w) + "|"] + [line(r) for r in rows[1:]])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build_dir", nargs="?", help="CMake build directory (wiped and rebuilt)")
    ap.add_argument("--baseline", help="result file to compare against")
    ap.add_argument("--compare", metavar="FILE", help="do not run anything: compare FILE with --baseline")
    ap.add_argument("--out", help="result name (default: git sha); written to results/NAME.json")
    ap.add_argument("--reps", type=int, default=21, help="coalbench samples per test (default 21)")
    ap.add_argument("--runs", type=int, default=1, help="coalbench invocations; medians of medians (default 1)")
    ap.add_argument("--boots", type=int, default=5, help="boot-time repetitions (default 5)")
    ap.add_argument("--memory", type=int, default=128, help="guest RAM in MiB (default 128)")
    ap.add_argument("--qemu-args", default="", help="extra QEMU arguments, e.g. '-icount shift=0,sleep=off'")
    args = ap.parse_args()

    base = json.load(open(args.baseline)) if args.baseline else None
    if args.compare:
        if not base:
            ap.error("--compare needs --baseline")
        print(compare(json.load(open(args.compare)), base))
        return 0
    if not args.build_dir:
        ap.error("BUILD_DIR is required")

    qemu_args = (QEMU_ARGS + " " + args.qemu_args).strip()
    mcp = Mcp(os.path.abspath(args.build_dir), args.memory, qemu_args)
    try:
        print("building ...", file=sys.stderr)
        mcp.tool("coal_build", clean=True, timeout=1800)
        res = {"git": git_info(), "date": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
               "env": environment(qemu_args, args.memory),
               "kernel": kernel_size(args.build_dir)}
        print("boot time x%d ..." % args.boots, file=sys.stderr)
        res["boot"] = measure_boot(mcp, args.boots)
        print("coalbench ...", file=sys.stderr)
        res["bench"] = run_coalbench(mcp, args.reps, args.runs)
    finally:
        mcp.close()

    RESULTS.mkdir(exist_ok=True)
    name = args.out or res["git"]["sha"] + ("-dirty" if res["git"]["dirty"] else "")
    path = RESULTS / (name + ".json")
    path.write_text(json.dumps(res, indent=1) + "\n")
    print("wrote", path, file=sys.stderr)
    print(compare(res, base) if base else values(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
