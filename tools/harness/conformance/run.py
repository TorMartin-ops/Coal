#!/usr/bin/env python3
"""Shell-level conformance corpus: the same cases run on a Linux host and inside Coal OS.

    run.py [--target host|coal|both] [--category NAME] [--json] [--verbose] [--build-dir DIR]

A case is a shell script and the stdout and exit status it must produce (see cases/).
The host run validates the corpus: a case that fails on Linux is a corpus bug (or a tool
the host lacks) and is left out of Coal's pass rate. Coal runs through coal-mcp's
coal_run, exactly as an agent drives it. Network cases talk to servers started here,
at 127.0.0.1 from the host and at QEMU's gateway 10.0.2.2 from Coal. Standard library only.
"""
import argparse
import difflib
import http.server
import json
import os
import re
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from mcpclient import McpClient  # noqa: E402

CASES = HERE / "cases"
CASE_TIMEOUT_MS = 20000
COAL_GATEWAY = "10.0.2.2"
COAL_SCRATCH = "/tmp/conformance"


class Case:
    def __init__(self, category, name, script, exit_code, stdout):
        self.category, self.name, self.script, self.exit_code, self.stdout = category, name, script, exit_code, stdout


def load_cases(directory, only=None):
    """Parses `== name`, script lines, `-- [exit]`, expected stdout lines."""
    cases = []
    for path in sorted(Path(directory).glob("*.txt")):
        if only and path.stem != only:
            continue
        name, script, expect, code, state = None, [], [], 0, "skip"
        for line in path.read_text().splitlines() + ["== "]:
            if line.startswith("== "):
                if name:
                    cases.append(Case(path.stem, name, "\n".join(script) + "\n", code,
                                      "".join(l + "\n" for l in expect)))
                name, script, expect, code, state = line[3:].strip(), [], [], 0, "script"
            elif state == "script" and line.startswith("--"):
                code, state = int(line[2:].strip() or 0), "expect"
            elif state == "script":
                script.append(line)
            elif state == "expect":
                expect.append(line)
    return cases


class NetServers:
    """TCP echo, UDP echo and a tiny HTTP server on ephemeral ports."""

    class Echo(socketserver.StreamRequestHandler):
        def handle(self):
            self.wfile.write(self.rfile.readline())

    class UdpEcho(socketserver.BaseRequestHandler):
        def handle(self):
            data, sock = self.request
            sock.sendto(data, self.client_address)

    class Http(http.server.BaseHTTPRequestHandler):
        BODIES = {"/hello.txt": b"hello-net\n", "/bin256": bytes(range(256)),
                  "/big": bytes(i * 7 & 255 for i in range(256 * 1024))}

        def do_GET(self):
            body = self.BODIES.get(self.path)
            self.send_response(200 if body is not None else 404)
            self.send_header("Content-Length", str(len(body or b"")))
            self.end_headers()
            self.wfile.write(body or b"")

        def log_message(self, *_):
            pass

    def __init__(self):
        socketserver.ThreadingTCPServer.allow_reuse_address = True
        self.servers = {"ECHO": socketserver.ThreadingTCPServer(("0.0.0.0", 0), self.Echo),
                        "UDP": socketserver.ThreadingUDPServer(("0.0.0.0", 0), self.UdpEcho),
                        "HTTP": http.server.ThreadingHTTPServer(("0.0.0.0", 0), self.Http)}
        for s in self.servers.values():
            s.daemon_threads = True
            threading.Thread(target=s.serve_forever, daemon=True).start()

    def substitutions(self, host):
        subs = {"HOST": host}
        subs.update({k: str(s.server_address[1]) for k, s in self.servers.items()})
        return subs

    def close(self):
        for s in self.servers.values():
            s.shutdown()
            s.server_close()


def expand(script, subs):
    return re.sub(r"@([A-Z]+)@", lambda m: subs.get(m.group(1), m.group(0)), script)


class HostTarget:
    name = "host"

    def __init__(self, servers):
        self.subs = servers.substitutions("127.0.0.1")
        self.tmp = tempfile.mkdtemp(prefix="coal-conformance-")

    def run(self, case):
        work = tempfile.mkdtemp(dir=self.tmp)
        try:
            r = subprocess.run(["/bin/sh", "-c", expand(case.script, self.subs)], cwd=work, stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=CASE_TIMEOUT_MS / 1000, env=dict(os.environ, LC_ALL="C"))
            return r.returncode, r.stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired:
            return None, "timed out"

    def close(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


class CoalTarget:
    name = "coal"
    RESULT = re.compile(r"exit code: (-?\d+)([^\n]*)(?:\n--- stdout ---\n(.*?))?(?:\n--- stderr ---\n.*)?\Z", re.S)

    def __init__(self, servers, build_dir):
        self.subs = servers.substitutions(COAL_GATEWAY)
        self.mcp = McpClient(build_dir, name="coal-conformance")
        self.mcp.tool("coal_boot", timeout_s=120)

    def run(self, case):
        script = "rm -rf %s; mkdir -p %s; cd %s || exit 99\n%s" % (COAL_SCRATCH, COAL_SCRATCH, COAL_SCRATCH, expand(case.script, self.subs))
        text = self.mcp.tool("coal_run", command=script, timeout_ms=CASE_TIMEOUT_MS)
        m = self.RESULT.match(text)
        if not m or "timed out" in m.group(2):
            return None, text
        return int(m.group(1)), m.group(3) or ""

    def close(self):
        self.mcp.close()


def judge(target, case):
    code, out = target.run(case)
    ok = code == case.exit_code and out == case.stdout
    return {"ok": ok, "exit": code, "stdout": out}


def explain(case, res):
    diff = "".join(difflib.unified_diff(case.stdout.splitlines(True), res["stdout"].splitlines(True),
                                        "expected", "actual", n=1))
    return "exit %s (expected %d)\n%s" % (res["exit"], case.exit_code, diff or "(stdout equal)")


def run_all(cases, targets, verbose):
    results = {}
    for case in cases:
        results[(case.category, case.name)] = {}
        for t in targets:
            res = judge(t, case)
            results[(case.category, case.name)][t.name] = res
            if verbose and not res["ok"]:
                print("[%s] FAIL %s/%s: %s" % (t.name, case.category, case.name, explain(case, res)), file=sys.stderr)
    return results


def tabulate(cases, results):
    """Per category: cases, valid on the host, Coal passes among the valid ones."""
    rows = {}
    for case in cases:
        r = results[(case.category, case.name)]
        row = rows.setdefault(case.category, {"cases": 0, "host_ok": 0, "coal_ok": 0, "coal_failed": [], "host_failed": []})
        row["cases"] += 1
        host_ok = r.get("host", {"ok": True})["ok"]
        row["host_ok"] += host_ok
        if not host_ok:
            row["host_failed"].append(case.name)
        if "coal" in r and host_ok:
            if r["coal"]["ok"]:
                row["coal_ok"] += 1
            else:
                row["coal_failed"].append(case.name)
    return rows


def render(rows, targets):
    both = "coal" in targets
    out = ["%-10s %6s %9s %s" % ("category", "cases", "host ok", "coal pass") if both else "%-10s %6s %9s" % ("category", "cases", "host ok")]
    for cat, r in rows.items():
        line = "%-10s %6d %9d" % (cat, r["cases"], r["host_ok"])
        if both:
            line += " %4d/%-3d %5.1f%%" % (r["coal_ok"], r["host_ok"], 100.0 * r["coal_ok"] / max(1, r["host_ok"]))
        out.append(line)
    if both:
        ok, valid = (sum(r[k] for r in rows.values()) for k in ("coal_ok", "host_ok"))
        out.append("%-10s %6d %9d %4d/%-3d %5.1f%%" % ("total", sum(r["cases"] for r in rows.values()), valid,
                                                      ok, valid, 100.0 * ok / max(1, valid)))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description="Coal OS shell conformance corpus")
    ap.add_argument("--target", choices=("host", "coal", "both"), default="both")
    ap.add_argument("--category", help="run only this category (file name in cases/)")
    ap.add_argument("--build-dir", default=os.path.join(tempfile.gettempdir(), "coal-conformance-build"),
                    help="CMake build directory used by coal-mcp")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--verbose", action="store_true", help="show the diff of every failure on stderr")
    args = ap.parse_args()
    cases = load_cases(CASES, args.category)
    if not cases:
        ap.error("no cases found")
    servers, targets = NetServers(), []
    try:
        if args.target in ("host", "both"):
            targets.append(HostTarget(servers))
        if args.target in ("coal", "both"):
            targets.append(CoalTarget(servers, args.build_dir))
        results = run_all(cases, targets, args.verbose)
    finally:
        for t in targets:
            t.close()
        servers.close()
    names = [t.name for t in targets]
    rows = tabulate(cases, results)
    if args.json:
        print(json.dumps({"targets": names, "categories": rows,
                          "cases": [{"category": c.category, "name": c.name,
                                     **{n: results[(c.category, c.name)][n]["ok"] for n in names}} for c in cases]},
                         indent=2))
    else:
        print(render(rows, names))
        for cat, r in rows.items():
            for label, key in (("invalid on host", "host_failed"), ("coal fails", "coal_failed")):
                if r[key]:
                    print("  %s, %s: %s" % (cat, label, ", ".join(r[key])))
    return 1 if any(r["host_ok"] != r["cases"] for r in rows.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
