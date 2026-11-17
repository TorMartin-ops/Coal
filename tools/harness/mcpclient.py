"""Minimal MCP client over the stdio of tools/coal-mcp/server.py (standard library only)."""
import json
import os
import select
import subprocess
import sys
import time

SERVER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "coal-mcp", "server.py")


class McpClient:
    def __init__(self, build_dir, memory_mb=None, env=None, name="coal-harness"):
        cmd = [sys.executable, SERVER, "--build-dir", str(build_dir)]
        if memory_mb:
            cmd += ["--memory", str(memory_mb)]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                  env=dict(os.environ, **(env or {})))
        self.id = 0
        self.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                 "clientInfo": {"name": name, "version": "1"}})
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _send(self, msg):
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()

    def call(self, method, params, timeout=900):
        self.id += 1
        self._send({"jsonrpc": "2.0", "id": self.id, "method": method, "params": params})
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.p.stdout], [], [], left)[0]:
                raise TimeoutError("no answer to %s within %ds" % (method, timeout))
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("coal-mcp exited")
            r = json.loads(line)
            if r.get("id") == self.id:
                return r

    def tool(self, name, timeout=900, **args):
        """Returns the text of a tool result; a tool error raises."""
        r = self.call("tools/call", {"name": name, "arguments": args}, timeout)
        if "error" in r:
            raise RuntimeError("%s: %s" % (name, r["error"]))
        text = "\n".join(c.get("text", "") for c in r["result"]["content"])
        if r["result"].get("isError"):
            raise RuntimeError("%s failed:\n%s" % (name, text))
        return text

    def close(self):
        try:
            self.tool("coal_shutdown", timeout=20)
        except Exception:
            pass
        self.p.stdin.close()
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill()
