#!/usr/bin/env python3
"""Which Linux syscalls do real workloads need that Coal OS does not implement?

    syscall_gap.py --collect DIR [--markdown FILE] [--json]    run the sample workloads under strace
    syscall_gap.py LOG... [--markdown FILE] [--json]            analyse strace -f -o / -c logs

Each log is one workload (the file name before the first dot names it). The set of
syscalls it uses is compared with Coal's table, parsed from kernel/cpu/syscall.c.
Coal and strace both use the x86_64 table, so names (and numbers) match directly.
Missing syscalls are ranked by how many workloads need them.
Standard library only.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TABLE_FILE = Path("kernel/cpu/syscall.c")
HANDLER_DIR = Path("kernel/cpu")

CALL = re.compile(r"^(?:\[pid\s+\d+\]\s+|\d+\s+)?(\w+)\(")
SUMMARY = re.compile(r"^\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+(?:\d+\s+)?(\w+)\s*$")

TINY_C = '#include <stdio.h>\nint main(void) { puts("hello"); return 0; }\n'
TINY_TEST = "def test_add():\n    assert 1 + 1 == 2\n\n\ndef test_str():\n    assert 'a' + 'b' == 'ab'\n"
TINY_MAKE = ".PHONY: all\nall: a b c d\n%:\n\t@echo $@ > $@.out\n"
WORKLOADS = [        # name, tool that must exist, setup files, shell command
    ("python3-c", "python3", {}, "python3 -c \"import json, os; print(json.dumps(sorted(os.listdir('.'))))\""),
    ("node-e", "node", {}, "node -e \"console.log(require('fs').readdirSync('.').length)\""),
    ("gcc-hello", "gcc", {"hello.c": TINY_C}, "gcc hello.c -o hello"),
    ("git-init-commit-log", "git", {"f.txt": "x\n"},
     "git init -q && git add f.txt && git -c user.name=t -c user.email=t@t commit -q -m one && git log --oneline"),
    ("pytest-tiny", "pytest", {"test_tiny.py": TINY_TEST}, "pytest -q -p no:cacheprovider"),
    ("make-j", "make", {"Makefile": TINY_MAKE}, "make -j4"),
]


def parse_table(src):
    """Returns ({syscall name: handler}, {handler: [lines that return -ENOSYS]}) from the kernel sources."""
    text = (Path(src) / TABLE_FILE).read_text()
    table = {m.group(1).lower(): m.group(2)
             for m in re.finditer(r"\[\s*SYS_(\w+)\s*\]\s*=\s*SC\((\w+)\)", text)}
    partial = {}
    for path in sorted((Path(src) / HANDLER_DIR).glob("*.c")):
        handler = None
        for line in path.read_text().splitlines():
            m = re.match(r"^(?:static\s+)?long\s+(sc_\w+)\(", line)
            handler = m.group(1) if m else handler
            if handler and "-ENOSYS" in line:
                partial.setdefault(handler, []).append(line.strip())
    return table, partial


def parse_strace(path):
    """Syscall name -> call count, from a full strace log or a `strace -c` summary."""
    counts = Counter()
    for line in Path(path).read_text(errors="replace").splitlines():
        m = SUMMARY.match(line)
        if m:
            counts[m.group(2)] += int(m.group(1))
            continue
        m = CALL.match(line)
        if m and "resumed>" not in line:
            counts[m.group(1)] += 1
    counts.pop("total", None)
    return counts


def collect(directory):
    """Runs each installed workload under strace -f; returns the log paths."""
    if shutil.which("strace") is None:
        raise SystemExit("strace is not installed (apt install strace)")
    logs = []
    for name, tool, files, command in WORKLOADS:
        if shutil.which(tool) is None:
            print("skip %s: %s not installed" % (name, tool), file=sys.stderr)
            continue
        with tempfile.TemporaryDirectory(prefix="gap-") as work:
            for fname, body in files.items():
                Path(work, fname).write_text(body)
            log = Path(directory, name + ".strace")
            r = subprocess.run(["strace", "-f", "-o", str(log), "sh", "-c", command], cwd=work,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode:
            print("note %s: workload exited %d" % (name, r.returncode), file=sys.stderr)
        logs.append(log)
    return logs


def analyse(logs, table, partial):
    """Compares every workload with Coal's table; returns the per-workload and ranked results."""
    coal = set(table)
    workloads, needs = {}, {}
    for log in logs:
        name = Path(log).name.split(".")[0]
        used = parse_strace(log)
        workloads[name] = {"used": len(used), "missing": sorted(n for n in used if n not in coal)}
        for n, c in used.items():
            if n not in coal:
                need = needs.setdefault(n, {"workloads": [], "calls": 0})
                need["workloads"].append(name)
                need["calls"] += c
    ranked = sorted(needs.items(), key=lambda kv: (-len(kv[1]["workloads"]), -kv[1]["calls"], kv[0]))
    stubbed = {n: partial[h] for n, h in table.items() if h in partial}
    return {"coal_syscalls": len(coal), "workloads": workloads,
            "missing": [{"name": n, "workloads": v["workloads"], "calls": v["calls"]} for n, v in ranked],
            "partial": {n: sorted(set(lines)) for n, lines in sorted(stubbed.items())}}


def summary(res, top):
    out = ["Coal implements %d syscalls (by name)" % res["coal_syscalls"], ""]
    out.append("%-24s %6s %8s %9s" % ("workload", "used", "missing", "coverage"))
    for name, w in res["workloads"].items():
        out.append("%-24s %6d %8d %8.1f%%" % (name, w["used"], len(w["missing"]),
                                              100.0 * (w["used"] - len(w["missing"])) / max(1, w["used"])))
    out += ["", "most needed missing syscalls (workloads, calls):"]
    out += ["  %-22s %d/%d  %d" % (m["name"], len(m["workloads"]), len(res["workloads"]), m["calls"])
            for m in res["missing"][:top]]
    return "\n".join(out)


def markdown(res, top=None):
    n = len(res["workloads"])
    out = ["# Syscall gap: what real workloads need that Coal does not implement", "",
           "Generated by `tools/harness/syscall_gap.py`. Each workload ran on x86_64 Linux under `strace -f`; the",
           "syscalls it used are compared by name with Coal's table (`kernel/cpu/syscall.c`, the same x86_64 numbers).",
           "Workloads start through `sh -c`, as an agent runs them, so the shell's own calls count too.",
           "To regenerate: `tools/harness/syscall_gap.py --collect /tmp/gap-logs "
           "--markdown docs/compat/syscall-gap.md`.", "",
           "Coal implements %d syscalls by this count. A name in the table can still fail for some arguments; "
           "see the last section." % res["coal_syscalls"], "", "## Workloads", "",
           "| Workload | Syscalls used | Missing | Coverage |", "| --- | ---: | ---: | ---: |"]
    for name, w in res["workloads"].items():
        cov = 100.0 * (w["used"] - len(w["missing"])) / max(1, w["used"])
        out.append("| %s | %d | %d | %.1f%% |" % (name, w["used"], len(w["missing"]), cov))
    out += ["", "## Missing syscalls, ranked by workloads needing them", "",
            "| # | Syscall | Workloads | Calls | Needed by |", "| ---: | --- | ---: | ---: | --- |"]
    for i, m in enumerate(res["missing"][:top], 1):
        out.append("| %d | `%s` | %d/%d | %d | %s |" % (i, m["name"], len(m["workloads"]), n, m["calls"],
                                                       ", ".join(m["workloads"])))
    out += ["", "## In the table but incomplete", "",
            "These return `-ENOSYS` for some arguments, so a workload using them can still fail:", ""]
    out += ["- `%s`: %s" % (name, "; ".join("`%s`" % l for l in lines)) for name, lines in res["partial"].items()]
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description="Syscalls real workloads need that Coal OS lacks")
    ap.add_argument("logs", nargs="*", help="strace logs, one workload each")
    ap.add_argument("--collect", metavar="DIR", help="run the sample workloads under strace, logs go to DIR")
    ap.add_argument("--kernel-src", default=str(REPO / "src" / "CoalOS"))
    ap.add_argument("--markdown", metavar="FILE", help="write the full report as Markdown")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    ap.add_argument("--top", type=int, default=25, help="rows in the printed summary (default 25)")
    args = ap.parse_args()
    logs = list(args.logs)
    if args.collect:
        os.makedirs(args.collect, exist_ok=True)
        logs += collect(args.collect)
    if not logs:
        ap.error("give strace logs or --collect DIR")
    table, partial = parse_table(args.kernel_src)
    res = analyse(logs, table, partial)
    if args.markdown:
        Path(args.markdown).parent.mkdir(parents=True, exist_ok=True)
        Path(args.markdown).write_text(markdown(res))
    print(json.dumps(res, indent=2) if args.json else summary(res, args.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
