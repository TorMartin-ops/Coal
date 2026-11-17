#!/usr/bin/env python3
"""QEMU smoke test: builds Coal OS, boots it through coal-mcp and runs a few commands.

Usage: smoke_test.py [build-dir]    exits non-zero on the first failure.
"""
import functools
import hashlib
import http.server
import threading
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "server.py")


class Client:
    def __init__(self, build_dir):
        self.p = subprocess.Popen([sys.executable, SERVER, "--build-dir", build_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.n = 0
        self.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "smoke", "version": "1"}})

    def rpc(self, method, params):
        self.n += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        while True:
            msg = json.loads(self.p.stdout.readline())
            if msg.get("id") == self.n:
                return msg

    def tool(self, tool, **args):
        r = self.rpc("tools/call", {"name": tool, "arguments": args})["result"]
        return r["isError"], r["content"][0]["text"]


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        print(detail)
        raise SystemExit(1)


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(tempfile.gettempdir(), "coal-smoke")
    www = tempfile.mkdtemp(prefix="coal-www-")
    with open(os.path.join(www, "hello.txt"), "w") as f:
        f.write("hello from smoke\n")
    srv = http.server.ThreadingHTTPServer(
        ("0.0.0.0", 0), functools.partial(http.server.SimpleHTTPRequestHandler, directory=www))
    http_port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    c = Client(build_dir)
    try:
        err, text = c.tool("coal_boot")
        check(not err and "mcpd is ready" in text, "boot", text)
        err, text = c.tool("coal_run", command="uname -a; echo hello | wc -c")
        check(not err and "Coal" in text and "exit code: 0" in text, "run uname and a pipe", text)
        err, text = c.tool("coal_run", command="echo data > /tmp/f && cat /tmp/f && ls /bin")
        check("data" in text and "coalbox" in text, "write, read and list files", text)
        err, text = c.tool("coal_run", command="/usr/bin/syscall_test", timeout_ms=60000)
        check("0 failed" in text, "syscall_test", text)
        # --- kernel: CPU exceptions in user mode kill only the process ---
        err, text = c.tool("coal_run", command="/usr/bin/fault_test", timeout_ms=60000)
        check("0 failed" in text, "fault_test: user-mode exceptions become signals", text)
        err, text = c.tool("coal_run", command="echo still alive")
        check("still alive" in text, "kernel survived the faults", text)
        err, text = c.tool("coal_run", command="/usr/bin/fs_stress quick", timeout_ms=120000)
        check("0 failed" in text, "fs_stress: file I/O round trip and unlink/rename of open files", text)
        # --- end kernel ---
        err, text = c.tool("coal_run", command="dmesg")
        for _ in range(20):                 # DHCP needs about a second after boot
            if "lease:" in text or "static" in text:
                break
            time.sleep(0.5)
            err, text = c.tool("coal_run", command="dmesg")
        check("vfs: mounted fat on /" in text, "dmesg shows the kernel log", text)
        check("lease:" in text or "static" in text, "network configured (dhcp or static)", text)
        err, text = c.tool("coal_run", command="ping -c 1 10.0.2.2", timeout_ms=15000)
        check("exit code: 0" in text, "ping the QEMU gateway", text)
        err, text = c.tool("coal_run", command="wget -q -O - http://10.0.2.2:%d/hello.txt" % http_port, timeout_ms=30000)
        check("hello from smoke" in text, "wget from a host http.server", text)
        err, text = c.tool("coal_run", command="wget -O /tmp/x https://example.com/; echo rc=$?", timeout_ms=15000)
        check("not supported" in text, "https gives a clear error", text)
        err, text = c.tool("coal_run", command="false; echo rc=$?; sh -c 'exit 3'; echo rc=$?; echo a | grep b; echo rc=$?")
        check(text.split("--- stdout ---\n")[1].split() == ["rc=1", "rc=3", "rc=1"], "$? is expanded when each command runs", text)
        err, text = c.tool("coal_run", command="sleep 3000 | sleep 3001", timeout_ms=1000)
        check("timed out" in text, "timeout kills a pipeline", text)
        err, text = c.tool("coal_run", command="ps")
        check("sleep" not in text, "no orphans after a timeout (the process group was killed)", text)
        err, text = c.tool("coal_run", command="sleep 2 & echo started", timeout_ms=1500)
        check("started" in text and "timed out" not in text, "a background job does not hold coal_run", text)
        err, text = c.tool("coal_run", command="yes 0123456789 | head -n 100000", timeout_ms=20000)
        check("exit code: 0" in text and "output truncated" in text, "large stdout is capped, not hung", text[:200])
        blob = os.urandom(3 * 1024 * 1024)
        src, back = os.path.join(www, "blob.bin"), os.path.join(www, "blob.back")
        with open(src, "wb") as f:
            f.write(blob)
        err, text = c.tool("coal_write_file", path="/tmp/blob.bin", host_path=src)
        check(not err, "write a 3 MiB file", text)
        err, text = c.tool("coal_read_file", path="/tmp/blob.bin", save_to=back)
        with open(back, "rb") as f:
            got = f.read()
        check(not err and hashlib.sha256(got).digest() == hashlib.sha256(blob).digest(),
              "3 MiB read back byte for byte (%d bytes)" % len(got), text)
        err, text = c.tool("coal_run", command="rm /tmp/blob.bin")
        # --- sh and applets (scripts live in src/CoalOS/userspace/tests) ---
        for script in ("sh_test.sh", "applet_test.sh"):
            err, text = c.tool("coal_run", command="/bin/sh /usr/bin/" + script, timeout_ms=120000)
            check("exit code: 0" in text and " 0 failed" in text, script, text[-1500:])
        err, text = c.tool("coal_snapshot", name="smoke")
        check(not err, "snapshot", text)
        err, text = c.tool("coal_run", command="rm /tmp/f && echo gone")
        check("gone" in text, "delete a file", text)
        err, text = c.tool("coal_restore", name="smoke")
        check(not err and "mcpd is ready" in text, "restore reboots", text)
        err, text = c.tool("coal_read_file", path="/tmp/f")
        check(not err and "data" in text, "restore brought the file back", text)
        err, text = c.tool("coal_sysinfo")
        check("/proc/uptime" in text, "sysinfo", text)
    finally:
        c.tool("coal_shutdown")
        c.p.kill()
    print("smoke test passed")


if __name__ == "__main__":
    main()
