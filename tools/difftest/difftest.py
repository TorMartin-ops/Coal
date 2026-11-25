#!/usr/bin/env python3
"""Differential syscall testing: run each corpus program on the Linux host and inside Coal, diff the traces.

    difftest.py [--only NAME ...] [--out DIR] [--json FILE] [--build-dir DIR] [--host-only]
    difftest.py fuzz [--seeds N] ...            (see fuzz.py)

As a module: run(machine, only=None, out=None) -> result dict (JSON serialisable). Standard library only;
the C programs are built with musl-gcc (static, x86_64)..
"""
import argparse
import difflib
import json
import os
import re
import signal
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "corpus")
GUEST_DIR = "/tmp/dtbin"
CC = ["musl-gcc", "-static", "-O1", "-w", "-Werror=implicit-function-declaration", "-fno-stack-protector"]


SKIP = {"fsops"}                 # driven by fuzz.py, not a fixed program


def programs(only=None):
    names = sorted(f[:-2] for f in os.listdir(CORPUS) if f.endswith(".c") and f[:-2] not in SKIP)
    return [n for n in names if not only or n in only]


def build(names, out):
    os.makedirs(out, exist_ok=True)
    for n in names:
        src, exe = os.path.join(CORPUS, n + ".c"), os.path.join(out, n)
        if os.path.exists(exe) and os.path.getmtime(exe) >= max(os.path.getmtime(src), os.path.getmtime(os.path.join(CORPUS, "trace.h"))):
            continue
        subprocess.run(CC + ["-I" + CORPUS, "-o", exe, src], check=True)


def default_signals():
    """preexec_fn: a host run must not inherit ignored or blocked signals (nohup, background jobs) from the harness."""
    for n in range(1, signal.NSIG):
        if n not in (signal.SIGKILL, signal.SIGSTOP):
            try:
                signal.signal(n, signal.SIG_DFL)
            except (OSError, ValueError):
                pass
    signal.pthread_sigmask(signal.SIG_SETMASK, [])


def run_host(exe, timeout=30, args=()):
    try:
        p = subprocess.run([exe, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL, timeout=timeout,
                           preexec_fn=default_signals)
    except subprocess.TimeoutExpired as e:
        return {"exit_code": None, "signal": None, "stdout": (e.stdout or b"").decode("utf-8", "replace"), "timed_out": True}
    sig = -p.returncode if p.returncode < 0 else None
    return {"exit_code": None if sig else p.returncode, "signal": sig, "stdout": p.stdout.decode("utf-8", "replace"), "timed_out": False}


def run_guest(machine, name, timeout=30, args=()):
    r = machine.run("%s/%s %s" % (GUEST_DIR, name, " ".join(args)), timeout_s=timeout)
    sig = r.get("signal")
    if isinstance(sig, str) and sig.isdigit():
        sig = int(sig)
    return {"exit_code": r.get("exit_code"), "signal": sig, "stdout": r.get("stdout", ""), "timed_out": r.get("timed_out", False)}


LINE = re.compile(r"^(?P<label>[^:]+): (?P<res>\S+)(?: (?P<detail>.*))?$")


def parse(text):
    """Trace lines -> [(label, result, detail, raw)]; NOTE lines have no ': ' result and keep label = whole line."""
    out = []
    for raw in text.splitlines():
        m = LINE.match(raw)
        out.append((m["label"], m["res"], m["detail"] or "", raw) if m else (raw, "", "", raw))
    return out


def syscall_of(label):
    return re.match(r"[A-Za-z0-9_]+", label).group(0) if label else "?"


def classify(host, guest):
    """Classify one differing pair of trace lines."""
    if guest[1] == "ENOSYS" and host[1] != "ENOSYS":
        return "missing syscall"
    if host[1].startswith("E") and guest[1].startswith("E") and host[1] != guest[1]:
        return "wrong errno"
    return "wrong semantics"


def compare(host, guest):
    """Diff two runs of one program. Returns the divergences and a verdict."""
    h, g = parse(host["stdout"]), parse(guest["stdout"])
    divs = []
    sm = difflib.SequenceMatcher(None, [x[3] for x in h], [x[3] for x in g], autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        hs, gs = h[i1:i2], g[j1:j2]
        for k in range(max(len(hs), len(gs))):
            hl = hs[k] if k < len(hs) else None
            gl = gs[k] if k < len(gs) else None
            if hl and gl:
                kind = classify(hl, gl) if syscall_of(hl[0]) == syscall_of(gl[0]) else "wrong semantics"
                divs.append({"syscall": syscall_of(hl[0]), "kind": kind, "host": hl[3], "coal": gl[3]})
            elif hl:
                divs.append({"syscall": syscall_of(hl[0]), "kind": "missing line", "host": hl[3], "coal": None})
            else:
                divs.append({"syscall": syscall_of(gl[0]), "kind": "extra line", "host": None, "coal": gl[3]})
    crashed = bool(guest["signal"]) or guest["timed_out"]
    host_crashed = bool(host["signal"])
    if crashed and not host_crashed:
        # the lines after the last one the guest printed are lost to the crash, not separate divergences
        cut = len(g)
        divs = [d for d in divs if d["kind"] != "missing line"]
        divs.append({"syscall": syscall_of(h[cut][0]) if cut < len(h) else "exit", "kind": "crash",
                     "host": h[cut][3] if cut < len(h) else None,
                     "coal": "killed by signal %s" % guest["signal"] if guest["signal"] else "timed out"})
    elif guest["exit_code"] != host["exit_code"] and not host_crashed:
        divs.append({"syscall": "exit", "kind": "wrong semantics", "host": "exit %s" % host["exit_code"],
                     "coal": "exit %s" % guest["exit_code"]})
    return divs, len(h)


def run(machine, only=None, out=None, build_dir=None, host_only=False):
    names = programs(only)
    out = out or tempfile.mkdtemp(prefix="difftest-")
    bin_dir = build_dir or os.path.join(out, "bin")
    build(names, bin_dir)
    if not host_only:
        machine.run("mkdir -p %s" % GUEST_DIR)
        gone = machine.put_tree(bin_dir, GUEST_DIR, names) if hasattr(machine, "put_tree") else names
        for n in gone:
            machine.put(os.path.join(bin_dir, n), "%s/%s" % (GUEST_DIR, n))
    res = {"programs": {}}
    for n in names:
        exe = os.path.join(bin_dir, n)
        host = run_host(exe)
        entry = {"host_lines": len(parse(host["stdout"])), "host_exit": host["exit_code"], "host_signal": host["signal"]}
        if not host_only:
            guest = run_guest(machine, n)
            divs, total = compare(host, guest)
            entry.update(coal_lines=len(parse(guest["stdout"])), coal_exit=guest["exit_code"], coal_signal=guest["signal"],
                         divergences=divs, diverging=len(divs), total=total)
            with open(os.path.join(out, n + ".coal.trace"), "w") as f:
                f.write(guest["stdout"])
        with open(os.path.join(out, n + ".host.trace"), "w") as f:
            f.write(host["stdout"])
        res["programs"][n] = entry
        print("%-12s %s" % (n, "lines=%d diverging=%d" % (entry["host_lines"], entry.get("diverging", 0))), file=sys.stderr)
    res["summary"] = summarize(res["programs"])
    return res


def summarize(programs):
    by_call, by_kind = {}, {}
    for n, e in programs.items():
        for d in e.get("divergences", []):
            c = by_call.setdefault(d["syscall"], {"count": 0, "kinds": {}, "programs": []})
            c["count"] += 1
            c["kinds"][d["kind"]] = c["kinds"].get(d["kind"], 0) + 1
            if n not in c["programs"]:
                c["programs"].append(n)
            by_kind[d["kind"]] = by_kind.get(d["kind"], 0) + 1
    ranked = sorted(by_call.items(), key=lambda kv: (-len(kv[1]["programs"]), -kv[1]["count"], kv[0]))
    return {"by_kind": by_kind, "by_syscall": dict(ranked),
            "programs_clean": sorted(n for n, e in programs.items() if not e.get("diverging")),
            "lines_total": sum(e.get("total", 0) for e in programs.values()),
            "lines_diverging": sum(e.get("diverging", 0) for e in programs.values())}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--out", help="directory for binaries and traces (default: temp)")
    ap.add_argument("--json", help="write the result here")
    ap.add_argument("--build-dir", default=os.environ.get("COAL_BUILD_DIR", os.path.join(tempfile.gettempdir(), "difftest-coal")),
                    help="CMake build directory of Coal OS (built if missing)")
    ap.add_argument("--host-only", action="store_true")
    a = ap.parse_args(argv)
    m = None
    if not a.host_only:
        from machine import GatewayMachine
        m = GatewayMachine(a.build_dir)
    try:
        res = run(m, a.only, a.out, host_only=a.host_only)
    finally:
        if m:
            m.close()
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)
    print(json.dumps(res["summary"], indent=1))


if __name__ == "__main__":
    main()
