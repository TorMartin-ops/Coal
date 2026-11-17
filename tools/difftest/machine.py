"""Guest access for the difftest suites: the small interface a suite needs from a machine.

    run(command, timeout_s=60) -> {"exit_code", "signal", "stdout", "stderr", "timed_out"}
    put(host_path, guest_path, mode=0o755)

GatewayMachine implements it over tools/harness/mcpclient (coal-mcp). Another runner can pass any object with
the same two methods, so the suites stay plain modules with run(machine). QEMU must be started under
`flock /tmp/coal-testkit/slots/0` (the lock is held by the caller, not here).
"""
import contextlib
import fcntl
import functools
import http.server
import os
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))
from mcpclient import McpClient  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


SLOT = "/tmp/coal-testkit/slots/0"


@contextlib.contextmanager
def slot(path=SLOT):
    """Hold the host-wide guest slot (the lock `flock <slot> <command>` takes) for the duration of a block."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        yield


class GatewayMachine:
    def __init__(self, build_dir, memory_mb=256):
        self.client = McpClient(build_dir, memory_mb=memory_mb, name="difftest")
        self.client.tool("coal_boot", timeout=120)

    def run(self, command, timeout_s=60, cwd=None):
        args = {"command": command, "timeout_ms": int(timeout_s * 1000), "head_bytes": 200000, "tail_bytes": 200000}
        if cwd:
            args["cwd"] = cwd
        r = self.client.call("tools/call", {"name": "coal_run", "arguments": args}, timeout=timeout_s + 60)
        if "error" in r:
            raise RuntimeError(r["error"])
        s = r["result"].get("structuredContent") or {}
        return {"exit_code": s.get("exit_code"), "signal": s.get("signal"),
                "stdout": (s.get("stdout") or {}).get("text", ""), "stderr": (s.get("stderr") or {}).get("text", ""),
                "timed_out": bool(s.get("timed_out"))}

    def put(self, host_path, guest_path, mode=0o755):
        self.client.tool("coal_write_file", path=guest_path, host_path=host_path, mode=oct(mode)[2:])

    def put_tree(self, host_dir, guest_dir, rel_paths):
        """Copy many files: the guest fetches them from a short-lived HTTP server on the host (QEMU's gateway
        10.0.2.2); about seven times faster than coal_write_file. Returns the paths that did not arrive."""
        class Quiet(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *a):
                pass
        srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), functools.partial(Quiet, directory=host_dir))
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        port = srv.server_address[1]
        missing = []
        try:
            for i in range(0, len(rel_paths), 16):
                chunk = rel_paths[i:i + 16]
                cmd = "; ".join("mkdir -p %s/%s; wget -q -O %s/%s http://10.0.2.2:%d/%s && chmod 755 %s/%s" % (
                    guest_dir, os.path.dirname(p) or ".", guest_dir, p, port, p, guest_dir, p) for p in chunk)
                self.run(cmd, timeout_s=120)
                for p in chunk:
                    out = self.run("test -s %s/%s && echo ok" % (guest_dir, p))["stdout"]
                    if "ok" not in out:
                        missing.append(p)
        finally:
            srv.shutdown()
        return missing

    def close(self):
        self.client.close()
