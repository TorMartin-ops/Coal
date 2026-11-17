#!/usr/bin/env python3
"""LTP syscalls tests run inside Coal (and, for a baseline, on the Linux host in a private PID namespace).

    ltp.py [--ext DIR] [--only DIR ...] [--no-host] [--timeout S] [--json FILE] [--build-dir COALBUILD]

--ext is the output directory of build.sh (default build/difftest-external). Each test is one static program;
its verdict comes from the TPASS/TFAIL/TBROK/TCONF lines it prints (or the summary of the new LTP API). As a
module: run(machine, ext, only=None, host=True) -> JSON-serialisable dict. Run under `flock /tmp/coal-testkit/slots/0`.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))   # tools/difftest: machine.py
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
GUEST = "/tmp/ltp"
WORK = "/tmp/ltpwork"

TOKENS = {"pass": r"\bTPASS\b", "fail": r"\bTFAIL\b", "brok": r"\bTBROK\b", "conf": r"\bTCONF\b", "warn": r"\bTWARN\b"}
SUMMARY = {"pass": r"^passed\s+(\d+)", "fail": r"^failed\s+(\d+)", "brok": r"^broken\s+(\d+)", "conf": r"^skipped\s+(\d+)",
           "warn": r"^warnings\s+(\d+)"}


def parse(output, exit_code, signal, timed_out):
    """Counts and a verdict (pass, fail, brok, conf, crash, timeout, none) from one run's output."""
    counts = {k: len(re.findall(p, output)) for k, p in TOKENS.items()}
    if re.search(SUMMARY["pass"], output, re.M):
        counts = {k: int(re.search(p, output, re.M).group(1)) if re.search(p, output, re.M) else 0 for k, p in SUMMARY.items()}
    if timed_out:
        verdict = "timeout"
    elif signal:
        verdict = "crash"
    elif counts["brok"]:
        verdict = "brok"
    elif counts["fail"]:
        verdict = "fail"
    elif counts["pass"]:
        verdict = "pass"
    elif counts["conf"]:
        verdict = "conf"
    else:
        verdict = "none"
    first = next((l for l in output.splitlines() if re.search(r"TBROK|TFAIL|TCONF", l)), "")
    return {"verdict": verdict, "exit": exit_code, "signal": signal or None, **counts, "first": first[:200]}


def run_host(path, timeout):
    cmd = ["unshare", "-pf", "--mount-proc", "--uts", "--ipc", "timeout", "-s", "KILL", str(timeout), path]
    tmp = tempfile.mkdtemp(prefix="ltp-host-")
    try:
        p = subprocess.run(cmd, cwd=tmp, env=dict(os.environ, TMPDIR=tmp), stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout + 10)
        out, rc = p.stdout.decode("utf-8", "replace"), p.returncode
    except subprocess.TimeoutExpired:
        return parse("", None, None, True)
    finally:
        subprocess.run(["rm", "-rf", tmp])
    return parse(out, None if rc < 0 else (None if rc == 137 else rc), -rc if rc < 0 else (9 if rc == 137 else None), False)


def run_guest(machine, d, name, timeout):
    cmd = "rm -rf %s; mkdir -p %s %s/tmp; cd %s; TMPDIR=%s/tmp %s/%s/%s 2>&1; rc=$?; cd /; rm -rf %s" % (
        WORK, WORK, WORK, WORK, WORK, GUEST, d, name, WORK)
    r = machine.run(cmd, timeout_s=timeout)
    sig = r.get("signal")
    return parse(r.get("stdout", "") + r.get("stderr", ""), r.get("exit_code"), sig if sig else None, r.get("timed_out", False))


def run(machine, ext, only=None, host=True, timeout=30):
    root = os.path.join(ext, "ltp")
    dirs = sorted(d for d in os.listdir(root) if not only or d in only)
    res = {"suite": "ltp", "tests": {}}
    for d in dirs:
        names = sorted(f for f in os.listdir(os.path.join(root, d)) if os.path.isfile(os.path.join(root, d, f)))
        rel = ["%s/%s" % (d, n) for n in names]
        machine.run("rm -rf %s; mkdir -p %s/%s" % (GUEST, GUEST, d))
        gone = machine.put_tree(root, GUEST, rel)
        for n in names:
            test = "%s/%s" % (d, n)
            if test in gone:
                res["tests"][test] = {"coal": {"verdict": "none", "first": "upload failed"}}
                continue
            entry = {"coal": run_guest(machine, d, n, timeout)}
            if host:
                entry["host"] = run_host(os.path.join(root, d, n), timeout)
            res["tests"][test] = entry
            print("%-34s coal=%-7s host=%s" % (test, entry["coal"]["verdict"], entry.get("host", {}).get("verdict", "-")), file=sys.stderr)
        machine.run("rm -rf %s" % GUEST)
    res["summary"] = summarize(res["tests"])
    return res


def summarize(tests):
    by_dir, tot = {}, {"tests": 0, "host_pass": 0, "coal_pass_of_host_pass": 0, "coal_pass": 0}
    reasons = {}
    for t, e in tests.items():
        d = t.split("/")[0]
        c = e["coal"]
        s = by_dir.setdefault(d, {"tests": 0, "pass": 0, "fail": 0, "brok": 0, "conf": 0, "crash": 0, "timeout": 0, "none": 0,
                                  "tpass": 0, "tfail": 0, "tbrok": 0, "tconf": 0})
        s["tests"] += 1
        s[c["verdict"]] += 1
        for k in ("pass", "fail", "brok", "conf"):
            s["t" + k] += c.get(k, 0)
        tot["tests"] += 1
        tot["coal_pass"] += c["verdict"] == "pass"
        h = e.get("host")
        if h and h["verdict"] == "pass":
            tot["host_pass"] += 1
            tot["coal_pass_of_host_pass"] += c["verdict"] == "pass"
        if c["verdict"] in ("brok", "fail", "crash", "none") and (not h or h["verdict"] == "pass"):
            m = re.sub(r"/tmp/\S+|LTP_\w+|\d+", "N", re.sub(r"^\S+:\d+: ", "", c["first"]))
            reasons.setdefault(m, []).append(t)
    top = sorted(reasons.items(), key=lambda kv: -len(kv[1]))[:25]
    return {"totals": tot, "by_syscall": by_dir, "top_blockers": [{"reason": r, "tests": len(v), "examples": v[:4]} for r, v in top]}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ext", default=os.path.join(REPO, "build", "difftest-external"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--no-host", action="store_true")
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--batch", type=int, default=10, help="syscall directories per boot; the slot lock is held one batch at a time")
    ap.add_argument("--json")
    ap.add_argument("--build-dir", default=os.environ.get("COAL_BUILD_DIR", os.path.join(tempfile.gettempdir(), "difftest-coal")))
    a = ap.parse_args(argv)
    from machine import GatewayMachine, slot
    dirs = sorted(d for d in os.listdir(os.path.join(a.ext, "ltp")) if not a.only or d in a.only)
    res = {"suite": "ltp", "tests": {}}
    for i in range(0, len(dirs), a.batch):
        with slot():
            m = GatewayMachine(a.build_dir)
            try:
                res["tests"].update(run(m, a.ext, dirs[i:i + a.batch], not a.no_host, a.timeout)["tests"])
            finally:
                m.close()
    res["summary"] = summarize(res["tests"])
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)
    print(json.dumps(res["summary"]["totals"], indent=1))


if __name__ == "__main__":
    main()
