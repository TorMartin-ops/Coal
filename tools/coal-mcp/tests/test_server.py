#!/usr/bin/env python3
"""Tests for the stdio server (coal-mcp on the gateway core). Run: python3 tools/coal-mcp/tests/test_server.py

The end-to-end tests run the real agentd (built as a 32-bit Linux binary) on a
pty that is bridged to a unix socket, in place of QEMU's COM2.
"""
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "..", "server.py")
sys.path.insert(0, os.path.join(HERE, "..", "..", "coal-gateway", "tests"))

from support import HostAgent  # noqa: E402


def read(path):
    with open(path, "rb") as f:
        return f.read()


class Client:
    """Minimal MCP client over the server's stdio."""

    def __init__(self, *extra):
        self.build = tempfile.mkdtemp(prefix="coal-mcp-build-")
        self.p = subprocess.Popen([sys.executable, SERVER, "--build-dir", self.build, *extra],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE)
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

    def result(self, name, **args):
        return self.call("tools/call", {"name": name, "arguments": args})["result"]

    def tool(self, name, **args):
        r = self.result(name, **args)
        return r["isError"], r["content"][0]["text"]

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=10)
        self.p.stdout.close()
        shutil.rmtree(self.build, ignore_errors=True)


class InProcess(unittest.TestCase):
    """Gateway.handle called directly, to inject failures inside a tool."""

    def setUp(self):
        sys.path.insert(0, os.path.join(HERE, "..", "..", "coal-gateway"))
        from coal_gateway import cli, mcp, tools
        from coal_gateway.machine import Machine
        from coal_gateway.runlog import RunLog
        self.tmp = tempfile.mkdtemp(prefix="coal-mcp-inproc-")
        os.environ["COAL_BUILD_DIR"] = self.tmp
        machine = Machine()
        self.gw = mcp.Gateway(machine, RunLog(os.path.join(self.tmp, "runs"), lambda: machine.workspace_id),
                              {"name": "coal-mcp", "version": cli.repo_version()})
        self.tools = tools
        self.old = tools.BY_NAME["coal_ps"].fn
        tools.BY_NAME["coal_ps"].fn = lambda call, a: {}["exit"]

    def tearDown(self):
        self.tools.BY_NAME["coal_ps"].fn = self.old
        shutil.rmtree(self.tmp, ignore_errors=True)

    def call(self, method, params):
        return self.gw.handle({"jsonrpc": "2.0", "id": 1, "method": method, "params": params})

    def test_keyerror_in_a_tool_is_a_tool_result(self):
        r = self.call("tools/call", {"name": "coal_ps", "arguments": {}})
        self.assertNotIn("error", r)
        self.assertTrue(r["result"]["isError"])
        self.assertIn("KeyError", r["result"]["content"][0]["text"])

    def test_only_unknown_methods_are_32601(self):
        self.assertEqual(self.call("nope", {})["error"]["code"], -32601)

    def test_version_comes_from_the_repo(self):
        from coal_gateway.machine import SRC_DIR
        ver = read(os.path.join(SRC_DIR, "VERSION")).decode().strip()
        self.assertEqual(self.gw.server_info["version"], ver)

    def test_every_call_is_logged_with_its_run_id(self):
        self.call("tools/call", {"name": "coal_ps", "arguments": {"run_id": "r1"}})
        self.call("tools/call", {"name": "coal_status", "arguments": {"run_id": "r1"}})
        with open(os.path.join(self.tmp, "runs", "r1.jsonl")) as f:
            rows = [json.loads(l) for l in f]
        self.assertEqual([(r["seq"], r["tool"], r["is_error"]) for r in rows],
                         [(1, "coal_ps", True), (2, "coal_status", False)])
        self.assertEqual(rows[0]["workspace_id"], self.gw.machine.workspace_id)

    def test_bad_run_id_and_workspace_id(self):
        self.assertEqual(self.call("tools/call", {"name": "coal_status", "arguments": {"run_id": "../x"}})["error"]["code"], -32602)
        r = self.call("tools/call", {"name": "coal_status", "arguments": {"workspace_id": "ws-nope"}})["result"]
        self.assertTrue(r["isError"])
        self.assertIn("unknown workspace_id", r["content"][0]["text"])


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
                  "coal_screen", "coal_build", "coal_guide", "coal_snapshot", "coal_restore", "coal_reset",
                  "coal_spawn", "coal_jobs", "coal_job_status", "coal_job_output", "coal_job_signal",
                  "coal_stat", "coal_mkdir", "coal_remove", "coal_keys", "coal_status", "coal_download"):
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


class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host = HostAgent()
        cls.tmp = cls.host.tmp
        cls.c = Client("--attach", cls.host.address)

    @classmethod
    def tearDownClass(cls):
        cls.c.close()
        cls.host.close()

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

    def test_multi_megabyte_roundtrip_and_save_to_fetches_everything(self):
        payload = os.urandom(3 * 1024 * 1024 + 123)
        src = os.path.join(self.tmp, "src.bin")
        with open(src, "wb") as f:
            f.write(payload)
        path = os.path.join(self.tmp, "big.bin")
        err, text = self.c.tool("coal_write_file", path=path, host_path=src)
        self.assertFalse(err, text)
        self.assertEqual(read(path), payload)
        out = os.path.join(self.tmp, "big.out")
        err, text = self.c.tool("coal_read_file", path=path, save_to=out)      # no max_bytes: whole file
        self.assertFalse(err, text)
        self.assertEqual(read(out), payload)
        err, text = self.c.tool("coal_read_file", path=path, save_to=out, offset=1000000, max_bytes=200000)
        self.assertEqual(read(out), payload[1000000:1200000])

    def test_run_kills_the_whole_process_group_on_timeout(self):
        err, text = self.c.tool("coal_run", command="sleep 3137 & sleep 3138 | sleep 3139", timeout_ms=500)
        self.assertIn("timed out", text)
        left = subprocess.run(["pgrep", "-f", "^sleep 313[789]$"], stdout=subprocess.PIPE).stdout
        self.assertEqual(left, b"")

    def test_background_job_does_not_hold_the_call(self):
        import time
        t = time.time()
        err, text = self.c.tool("coal_run", command="sleep 20 & echo started", timeout_ms=10000)
        self.assertLess(time.time() - t, 5)
        self.assertIn("started", text)
        subprocess.run(["pkill", "-f", "^sleep 20$"])  # the job was left running on purpose

    def test_huge_output_never_hangs_and_is_flagged(self):
        err, text = self.c.tool("coal_run", command="head -c 3000000 /dev/zero | tr '\\0' x; echo done >&2",
                                timeout_ms=30000)
        self.assertIn("exit code: 0", text)
        self.assertIn("output truncated", text)
        self.assertIn("done", text)
        self.assertIn("of 3000000 bytes", text)
        self.assertLess(len(text), 20000)             # head and tail only; the rest is in the VM

    def test_errno_text_on_write_failure(self):
        err, text = self.c.tool("coal_write_file", path="/nonexistent-dir/x", content="x")
        self.assertTrue(err)
        self.assertIn("No such file or directory", text)
        self.assertIn("errno 2", text)

    def test_failed_write_leaves_no_partial_file(self):
        path = os.path.join(self.tmp, "keep.txt")
        self.c.tool("coal_write_file", path=path, content="original")
        err, text = self.c.tool("coal_write_file", path=path, content_base64="!!!")
        self.assertTrue(err)
        self.assertEqual(read(path), b"original")
        self.assertEqual([n for n in os.listdir(self.tmp) if n.endswith(".agentd-tmp")], [])

    def test_numbers_are_checked_not_clamped(self):
        err, text = self.c.tool("coal_run", command="true", timeout_ms="soon")
        self.assertTrue(err)
        self.assertIn("timeout_ms must be a number", text)
        err, text = self.c.tool("coal_read_file", path="/etc/hostname", offset="x")
        self.assertTrue(err)

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
        self.assertIn("agentd", text)
        err, text = self.c.tool("coal_ps")
        self.assertIn("PID", text)
        err, text = self.c.tool("coal_sysinfo")
        self.assertIn("Linux", text)

    def test_run_reports_a_structured_envelope(self):
        r = self.c.result("coal_run", command="echo hi; exit 3", cwd="/tmp", env={"X": "1"})
        d = r["structuredContent"]
        self.assertEqual((d["exit_code"], d["signal"], d["timed_out"], d["killed_by"], d["cwd"]),
                         (3, 0, False, None, "/tmp"))
        self.assertEqual((d["stdout"]["text"], d["stdout"]["bytes_total"], d["stdout"]["truncated"]), ("hi\n", 3, False))
        self.assertIn("duration_ms", d)
        t = self.c.result("coal_run", command="sleep 30", timeout_ms=300)["structuredContent"]
        self.assertEqual((t["timed_out"], t["killed_by"]), (True, "timeout"))

    def test_no_output_placeholder_and_noninteractive_env(self):
        err, text = self.c.tool("coal_run", command="true")
        self.assertIn("(no output)", text)
        err, text = self.c.tool("coal_run", command='echo "$PAGER $EDITOR $CI $TERM $GIT_TERMINAL_PROMPT"; cat; echo eof')
        self.assertIn("cat true 1 dumb 0", text)
        self.assertIn("eof", text)                       # cat saw /dev/null and ended

    def test_job_lifecycle(self):
        err, text = self.c.tool("coal_spawn", command="echo started; sleep 0.3; echo finished")
        self.assertFalse(err, text)
        handle = self.c.result("coal_jobs")["structuredContent"]["jobs"][0]["job_id"]
        self.assertRegex(handle, r"j\d+-\d+")
        err, text = self.c.tool("coal_job_status", job_id=handle, wait_ms=5000)
        self.assertIn("exited, exit code 0", text)
        err, text = self.c.tool("coal_job_output", job_id=handle)
        self.assertIn("started\nfinished", text)
        err, text = self.c.tool("coal_job_output", job_id=handle, offset=8)
        self.assertTrue(text.endswith("finished\n"), text)
        err, text = self.c.tool("coal_job_status", job_id=handle, release=True)
        self.assertIn("released", text)
        err, text = self.c.tool("coal_job_status", job_id=handle)
        self.assertTrue(err)
        err, text = self.c.tool("coal_job_status", job_id="nonsense")
        self.assertTrue(err)

    def test_job_signal(self):
        self.c.tool("coal_spawn", command="sleep 60")
        handle = self.c.result("coal_jobs")["structuredContent"]["jobs"][-1]["job_id"]
        err, text = self.c.tool("coal_job_signal", job_id=handle)
        self.assertFalse(err, text)
        err, text = self.c.tool("coal_job_status", job_id=handle, wait_ms=5000)
        self.assertIn("killed by signal", text)

    def test_stat_mkdir_remove(self):
        d = os.path.join(self.tmp, "a", "b")
        err, text = self.c.tool("coal_mkdir", path=d, parents=True)
        self.assertFalse(err, text)
        s = self.c.result("coal_stat", path=d)["structuredContent"]
        self.assertEqual(s["type"], "dir")
        err, text = self.c.tool("coal_remove", path=os.path.join(self.tmp, "a"))
        self.assertTrue(err)                              # not empty
        err, text = self.c.tool("coal_remove", path=os.path.join(self.tmp, "a"), recursive=True)
        self.assertFalse(err, text)
        err, text = self.c.tool("coal_stat", path=d)
        self.assertTrue(err)

    def test_survives_reconnect(self):
        self.c.tool("coal_run", command="true")
        self.c.close()
        type(self).c = Client("--attach", self.host.address)
        err, text = self.c.tool("coal_run", command="echo again")
        self.assertIn("again", text)


if __name__ == "__main__":
    unittest.main()
