#!/usr/bin/env python3
"""Tests for coal-mcp. Run: python3 tools/coal-mcp/tests/test_server.py

The end-to-end tests run the real mcpd (built as a 32-bit Linux binary) on a
pty that is bridged to a unix socket, in place of QEMU's COM2.
"""
import base64
import json
import os
import pty
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import tty
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "server.py")
MCPD_BUILD = os.path.join(HERE, "..", "..", "..", "src", "CoalOS", "userspace", "mcpd", "test", "build_host.sh")


def read(path):
    with open(path, "rb") as f:
        return f.read()


class Client:
    """Minimal MCP client over the server's stdio."""

    def __init__(self, *extra):
        self.p = subprocess.Popen([sys.executable, SERVER, *extra], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE)
        self.n = 0

    def send(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            self.n += 1
            msg["id"] = self.n
        self.p.stdin.write(json.dumps(msg).encode() + b"\n")
        self.p.stdin.flush()

    def recv(self):
        return json.loads(self.p.stdout.readline())

    def call(self, method, params=None):
        self.send(method, params)
        return self.recv()

    def tool(self, name, **args):
        r = self.call("tools/call", {"name": name, "arguments": args})["result"]
        return r["isError"], r["content"][0]["text"]

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=10)
        self.p.stdout.close()


class Protocol(unittest.TestCase):
    def setUp(self):
        self.c = Client()

    def tearDown(self):
        self.c.close()

    def test_legacy_handshake(self):
        r = self.c.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "t", "version": "0"}})["result"]
        self.assertEqual(r["protocolVersion"], "2025-06-18")
        self.assertEqual(r["serverInfo"]["name"], "coal-mcp")
        self.assertIn("tools", r["capabilities"])
        self.c.send("notifications/initialized", notify=True)
        self.assertEqual(self.c.call("ping")["result"], {})

    def test_unknown_version_falls_back_to_latest_legacy(self):
        r = self.c.call("initialize", {"protocolVersion": "1999-01-01"})["result"]
        self.assertEqual(r["protocolVersion"], "2025-11-25")

    def test_tools_list(self):
        tools = self.c.call("tools/list")["result"]["tools"]
        names = {t["name"] for t in tools}
        for n in ("coal_boot", "coal_shutdown", "coal_reboot", "coal_run", "coal_read_file",
                  "coal_write_file", "coal_ls", "coal_ps", "coal_sysinfo", "coal_kernel_log",
                  "coal_screen", "coal_build", "coal_guide", "coal_snapshot", "coal_restore", "coal_reset"):
            self.assertIn(n, names)
        for t in tools:
            self.assertEqual(t["inputSchema"]["type"], "object")
            self.assertTrue(t["description"])

    def test_guide_resource_and_instructions(self):
        init = self.c.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                          "clientInfo": {"name": "t", "version": "1"}})["result"]
        self.assertIn("coal_snapshot", init["instructions"])
        self.assertIn("resources", init["capabilities"])
        uri = self.c.call("resources/list")["result"]["resources"][0]["uri"]
        text = self.c.call("resources/read", {"uri": uri})["result"]["contents"][0]["text"]
        self.assertIn("RECOVERY", text)
        err, text = self.c.tool("coal_guide")
        self.assertFalse(err)
        self.assertIn("/sbin/init", text)

    def test_snapshot_names_are_validated(self):
        for bad in ("../x", "a/b", "", ".hidden", "x" * 65):
            r = self.c.call("tools/call", {"name": "coal_snapshot", "arguments": {"name": bad}})["result"]
            err = r["isError"]
            self.assertTrue(err, bad)

    def test_modern_era(self):
        meta = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                "io.modelcontextprotocol/clientCapabilities": {}}
        r = self.c.call("server/discover", {"_meta": meta})["result"]
        self.assertEqual(r["resultType"], "complete")
        self.assertIn("2026-07-28", r["supportedVersions"])
        r = self.c.call("tools/list", {"_meta": meta})["result"]
        self.assertEqual(r["resultType"], "complete")
        bad = dict(meta, **{"io.modelcontextprotocol/protocolVersion": "1900-01-01"})
        e = self.c.call("tools/list", {"_meta": bad})["error"]
        self.assertEqual(e["code"], -32022)
        self.assertIn("2026-07-28", e["data"]["supported"])

    def test_errors(self):
        self.assertEqual(self.c.call("no/such")["error"]["code"], -32601)
        self.assertEqual(self.c.call("tools/call", {"name": "nope"})["error"]["code"], -32602)
        err, text = self.c.tool("coal_run", command="true")      # VM not booted
        self.assertTrue(err)
        self.assertIn("coal_boot", text)
        self.c.p.stdin.write(b"not json\n")
        self.c.p.stdin.flush()
        self.assertEqual(self.c.recv()["error"]["code"], -32700)


def bridge(sock_path, master):
    """Forwards bytes between a unix socket client and a pty master."""
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(sock_path)
    srv.listen(1)

    def run():
        while True:
            conn, _ = srv.accept()
            while True:
                r, _, _ = select.select([conn, master], [], [])
                try:
                    if conn in r:
                        d = conn.recv(65536)
                        if not d:
                            break
                        os.write(master, d)
                    if master in r:
                        conn.sendall(os.read(master, 65536))
                except OSError:
                    break
            conn.close()
    threading.Thread(target=run, daemon=True).start()


@unittest.skipUnless(shutil.which("gcc"), "gcc not available")
class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="coal-mcp-test-")
        binary = os.path.join(cls.tmp, "mcpd")
        if subprocess.call(["sh", MCPD_BUILD, binary], stderr=subprocess.DEVNULL) != 0:
            raise unittest.SkipTest("cannot build 32-bit mcpd on this host")
        master, slave = pty.openpty()
        tty.setraw(master)
        cls.mcpd = subprocess.Popen([binary, os.ttyname(slave)])
        sock = os.path.join(cls.tmp, "com2.sock")
        bridge(sock, master)
        cls.c = Client("--attach", "unix:" + sock)

    @classmethod
    def tearDownClass(cls):
        cls.c.close()
        cls.mcpd.kill()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_run(self):
        err, text = self.c.tool("coal_run", command="echo out; echo err >&2; exit 7")
        self.assertFalse(err)
        self.assertIn("exit code: 7", text)
        self.assertIn("out", text)
        self.assertIn("--- stderr ---\nerr", text)

    def test_run_timeout(self):
        err, text = self.c.tool("coal_run", command="sleep 30", timeout_ms=400)
        self.assertFalse(err)
        self.assertIn("timed out", text)

    def test_file_roundtrip_binary_and_large(self):
        payload = bytes(range(256)) * 1000                    # 256000 bytes, several chunks
        path = os.path.join(self.tmp, "blob.bin")
        err, text = self.c.tool("coal_write_file", path=path,
                                content_base64=base64.b64encode(payload).decode())
        self.assertFalse(err, text)
        self.assertEqual(read(path), payload)
        out = os.path.join(self.tmp, "back.bin")
        err, text = self.c.tool("coal_read_file", path=path, max_bytes=len(payload), save_to=out)
        self.assertFalse(err, text)
        self.assertEqual(read(out), payload)

    def test_text_append_and_offset(self):
        path = os.path.join(self.tmp, "t.txt")
        self.c.tool("coal_write_file", path=path, content="héllo ")
        self.c.tool("coal_write_file", path=path, content="wörld", append=True)
        err, text = self.c.tool("coal_read_file", path=path)
        self.assertTrue(text.endswith("héllo wörld"), text)
        err, text = self.c.tool("coal_read_file", path=path, offset=7, max_bytes=1)
        self.assertTrue(text.endswith("\nw"), text)

    def test_missing_file_is_tool_error(self):
        err, text = self.c.tool("coal_read_file", path="/nonexistent/x")
        self.assertTrue(err)
        self.assertIn("errno 2", text)

    def test_ls_ps_sysinfo(self):
        err, text = self.c.tool("coal_ls", path=self.tmp)
        self.assertIn("mcpd", text)
        err, text = self.c.tool("coal_ps")
        self.assertIn("PID", text)
        err, text = self.c.tool("coal_sysinfo")
        self.assertIn("Linux", text)

    def test_survives_reconnect(self):
        self.c.tool("coal_run", command="true")
        self.c.close()
        type(self).c = Client("--attach", "unix:" + os.path.join(self.tmp, "com2.sock"))
        err, text = self.c.tool("coal_run", command="echo again")
        self.assertIn("again", text)


if __name__ == "__main__":
    unittest.main()
