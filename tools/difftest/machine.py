"""Guest access for the difftest suites: the small interface a suite needs from a machine.

    run(command, timeout_s=60) -> {"exit_code", "signal", "stdout", "stderr", "timed_out"}
    put(host_path, guest_path, mode=0o755)

GatewayMachine implements it over tools/harness/mcpclient (coal-mcp). Another runner can pass any object with
the same two methods, so the suites stay plain modules with run(machine). QEMU must be started under
`flock /tmp/coal-testkit/slots/0` (the lock is held by the caller, not here).
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))
from mcpclient import McpClient  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


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

    def close(self):
        self.client.close()
