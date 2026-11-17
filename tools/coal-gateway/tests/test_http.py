#!/usr/bin/env python3
"""HTTP transport, bearer auth, tasks and the run log, against agentd on a pty."""
import http.client
import json
import os
import shutil
import stat
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from support import HostAgent  # noqa: E402
from coal_gateway import auth  # noqa: E402
from coal_gateway.errors import ToolError  # noqa: E402
from coal_gateway.machine import Machine  # noqa: E402
from coal_gateway.mcp import Gateway  # noqa: E402
from coal_gateway.runlog import RunLog  # noqa: E402
from coal_gateway.streamable_http import HttpServer  # noqa: E402

TOKEN = "t" * 40
META = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {"extensions": {"io.modelcontextprotocol/tasks": {}}}}
META_NO_TASKS = {"io.modelcontextprotocol/protocolVersion": "2026-07-28", "io.modelcontextprotocol/clientCapabilities": {}}


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.host = HostAgent()
        cls.tmp = tempfile.mkdtemp(prefix="coal-http-")
        os.environ["COAL_BUILD_DIR"] = cls.tmp
        cls.machine = Machine(cls.host.address)
        cls.runs = os.path.join(cls.tmp, "runs")
        cls.gateway = Gateway(cls.machine, RunLog(cls.runs, lambda: cls.machine.workspace_id),
                              {"name": "coal-gateway", "version": "test"})
        cls.server = HttpServer(cls.gateway, auth.BearerToken(TOKEN), "127.0.0.1", 0)
        cls.server.start()
        cls.host_, cls.port = cls.server.address

    @classmethod
    def tearDownClass(cls):
        cls.server.close()
        cls.host.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def post(self, body, headers=None, token=TOKEN, raw=None, path="/mcp", method="POST"):
        conn = http.client.HTTPConnection(self.host_, self.port, timeout=30)
        h = {"Content-Type": "application/json"}
        if token:
            h["Authorization"] = "Bearer " + token
        h.update(headers or {})
        data = raw if raw is not None else json.dumps(body)
        conn.request(method, path, data, h)
        r = conn.getresponse()
        payload = r.read()
        conn.close()
        return r.status, dict(r.getheaders()), (json.loads(payload) if payload else None)

    def rpc(self, method, params=None, meta=META, headers=None, **kw):
        params = dict(params or {})
        if meta is not None:
            params["_meta"] = meta
        h = {"MCP-Protocol-Version": "2026-07-28", "Mcp-Method": method}
        h.update(headers or {})
        return self.post({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}, h, **kw)

    def call(self, name, meta=META, **args):
        params = {"name": name, "arguments": args}
        return self.rpc("tools/call", params, meta, {"Mcp-Name": name})


class Http(Base):
    def test_discover_is_stateless_and_advertises_tasks(self):
        status, headers, body = self.rpc("server/discover")
        self.assertEqual(status, 200)
        r = body["result"]
        self.assertIn("2026-07-28", r["supportedVersions"])
        self.assertIn("io.modelcontextprotocol/tasks", r["capabilities"]["extensions"])
        self.assertEqual(r["resultType"], "complete")
        self.assertNotIn("Mcp-Session-Id", {k.title() for k in headers} | {k for k in headers})

    def test_tools_list_has_every_tool_with_the_handles(self):
        tools = self.rpc("tools/list")[2]["result"]["tools"]
        self.assertEqual(len(tools), 27)
        for t in tools:
            self.assertIn("run_id", t["inputSchema"]["properties"])
            self.assertIn("workspace_id", t["inputSchema"]["properties"])
        self.assertEqual([t["name"] for t in tools], [t["name"] for t in self.rpc("tools/list")[2]["result"]["tools"]])

    def test_tool_call_over_http(self):
        status, _, body = self.call("coal_run", command="echo via http", run_id="http-1")
        self.assertEqual(status, 200)
        r = body["result"]
        self.assertFalse(r["isError"])
        self.assertIn("via http", r["content"][0]["text"])
        self.assertEqual(r["structuredContent"]["exit_code"], 0)
        self.assertEqual(r["resultType"], "complete")

    def test_legacy_request_without_version_headers_is_tolerated(self):
        status, _, body = self.post({"jsonrpc": "2.0", "id": 7, "method": "tools/list"})
        self.assertEqual(status, 200)
        self.assertEqual(len(body["result"]["tools"]), 27)
        self.assertNotIn("resultType", body["result"])

    def test_notification_gets_202(self):
        status, _, body = self.post({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.assertEqual((status, body), (202, None))

    # -- auth and hardening --------------------------------------------------------

    def test_missing_or_wrong_token_is_401(self):
        for token in (None, "wrong", TOKEN + "x", ""):
            status, headers, body = self.rpc("tools/list", token=token)
            self.assertEqual(status, 401, token)
            self.assertIn("Bearer", headers["WWW-Authenticate"])
            self.assertNotIn("tools", json.dumps(body))

    def test_bad_origin_is_403_even_with_a_valid_token(self):
        status, _, _ = self.rpc("tools/list", headers={"Origin": "http://evil.example"})
        self.assertEqual(status, 403)
        status, _, _ = self.rpc("tools/list", headers={"Origin": "http://localhost:3000"})
        self.assertEqual(status, 200)

    def test_get_and_other_paths(self):
        self.assertEqual(self.post(None, raw="", method="GET")[0], 405)
        self.assertEqual(self.post(None, raw="", method="GET", token=None)[0], 401)
        self.assertEqual(self.post({}, path="/other")[0], 404)

    def test_body_problems(self):
        self.assertEqual(self.post(None, raw="{nope")[0], 400)
        status, _, body = self.post(None, raw="[]")
        self.assertEqual(status, 400)
        self.assertEqual(self.post(None, raw=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "nope"}))[2]["error"]["code"], -32601)

    def test_oversized_body_is_413(self):
        conn = http.client.HTTPConnection(self.host_, self.port, timeout=10)
        conn.putrequest("POST", "/mcp")
        conn.putheader("Authorization", "Bearer " + TOKEN)
        conn.putheader("Content-Length", str(64 * 1024 * 1024))
        conn.endheaders()
        self.assertEqual(conn.getresponse().status, 413)
        conn.close()

    def test_header_mismatch_is_32020(self):
        status, _, body = self.rpc("tools/list", headers={"Mcp-Method": "tools/call"})
        self.assertEqual((status, body["error"]["code"]), (400, -32020))
        status, _, body = self.call("coal_run", command="true", )
        self.assertEqual(status, 200)
        status, _, body = self.rpc("tools/call", {"name": "coal_run", "arguments": {"command": "true"}}, headers={"Mcp-Name": "coal_ps"})
        self.assertEqual((status, body["error"]["code"]), (400, -32020))
        status, _, body = self.rpc("tools/list", headers={"MCP-Protocol-Version": "2025-11-25"})
        self.assertEqual((status, body["error"]["code"]), (400, -32020))
        status, _, body = self.post({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {"_meta": META}},
                                    {"MCP-Protocol-Version": "2026-07-28"})          # Mcp-Method missing
        self.assertEqual((status, body["error"]["code"]), (400, -32020))

    def test_unsupported_version(self):
        bad = {"io.modelcontextprotocol/protocolVersion": "1900-01-01"}
        status, _, body = self.rpc("tools/list", meta=bad, headers={"MCP-Protocol-Version": "1900-01-01"})
        self.assertEqual(body["error"]["code"], -32022)

    def test_non_loopback_bind_needs_the_flag(self):
        with self.assertRaises(ValueError):
            HttpServer(self.gateway, auth.BearerToken(TOKEN), "0.0.0.0", 0)
        s = HttpServer(self.gateway, auth.BearerToken(TOKEN), "0.0.0.0", 0, allow_non_loopback=True)
        s.close()


class Tasks(Base):
    def test_a_long_run_becomes_a_task_that_completes(self):
        status, _, body = self.call("coal_run", command="sleep 1; echo finished", timeout_ms=40000, run_id="task-1")
        r = body["result"]
        self.assertEqual((status, r["resultType"], r["status"]), (200, "task", "working"))
        for key in ("taskId", "ttlMs", "pollIntervalMs", "createdAt", "lastUpdatedAt"):
            self.assertIn(key, r)
        self.assertEqual(r["ttlMs"], 3600 * 1000)
        deadline = time.time() + 20
        while True:
            got = self.rpc("tasks/get", {"taskId": r["taskId"]}, headers={"Mcp-Name": r["taskId"]})[2]["result"]
            if got["status"] != "working" or time.time() > deadline:
                break
            time.sleep(0.2)
        self.assertEqual((got["status"], got["resultType"]), ("completed", "complete"))
        self.assertIn("finished", got["result"]["content"][0]["text"])
        with open(os.path.join(self.runs, "task-1.jsonl")) as f:
            row = json.loads(f.readline())
        self.assertEqual(row["task_id"], r["taskId"])

    def test_short_calls_and_clients_without_the_extension_get_plain_results(self):
        self.assertEqual(self.call("coal_run", command="true", timeout_ms=5000)[2]["result"]["resultType"], "complete")
        self.assertEqual(self.call("coal_run", meta=META_NO_TASKS, command="true", timeout_ms=40000)[2]["result"]["resultType"], "complete")

    def test_cancel_stops_the_command(self):
        r = self.call("coal_run", command="sleep 3150", timeout_ms=100000)[2]["result"]
        time.sleep(0.4)
        status, _, body = self.rpc("tasks/cancel", {"taskId": r["taskId"]}, headers={"Mcp-Name": r["taskId"]})
        self.assertEqual((status, body["result"].get("resultType")), (200, "complete"))
        deadline = time.time() + 15
        while time.time() < deadline:
            got = self.rpc("tasks/get", {"taskId": r["taskId"]})[2]["result"]
            if got["status"] != "working":
                break
            time.sleep(0.2)
        self.assertEqual(got["status"], "cancelled")

    def test_unknown_task_and_update_ack(self):
        self.assertEqual(self.rpc("tasks/get", {"taskId": "nope"})[2]["error"]["code"], -32602)
        self.assertEqual(self.rpc("tasks/cancel", {"taskId": "nope"})[2]["error"]["code"], -32602)
        r = self.call("coal_run", command="sleep 1", timeout_ms=40000)[2]["result"]
        self.assertEqual(self.rpc("tasks/update", {"taskId": r["taskId"], "inputResponses": {}})[2]["result"]["resultType"], "complete")

    def test_a_failing_tool_gives_a_completed_task_with_an_error_result(self):
        r = self.call("coal_run", command="true", timeout_ms=40000, cwd=5)[2]
        # bad arguments fail before a task exists only if validated eagerly; either way the error is a tool result
        res = r["result"]
        if res.get("resultType") == "task":
            time.sleep(0.5)
            res = self.rpc("tasks/get", {"taskId": res["taskId"]})[2]["result"]["result"]
        self.assertTrue(res["isError"])


class RunLogging(Base):
    def test_calls_are_logged_per_run_without_secrets(self):
        self.call("coal_run", command="echo hi; true # password=hunter2", env={"API_KEY": "sk-secret"}, run_id="log-1")
        self.call("coal_write_file", path="/tmp/%d-x" % os.getpid(), content="top secret body", run_id="log-1")
        self.call("coal_status", run_id="log-1")
        with open(os.path.join(self.runs, "log-1.jsonl")) as f:
            text = f.read()
        rows = [json.loads(line) for line in text.splitlines()]
        self.assertEqual([r["seq"] for r in rows], [1, 2, 3])
        self.assertEqual([r["tool"] for r in rows], ["coal_run", "coal_write_file", "coal_status"])
        for leak in ("hunter2", "sk-secret", "top secret body"):
            self.assertNotIn(leak, text)
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.runs, "log-1.jsonl")).st_mode), 0o600)
        self.assertEqual(rows[0]["result"]["bytes"] > 0, True)
        self.assertIsInstance(rows[0]["duration_ms"], int)
        self.assertEqual(rows[0]["workspace_id"], self.machine.workspace_id)

    def test_default_run_when_no_run_id_is_given(self):
        self.call("coal_status")
        self.assertTrue(os.path.exists(os.path.join(self.runs, self.gateway.default_run + ".jsonl")))

    def test_workspace_id_is_checked(self):
        r = self.call("coal_status", workspace_id="ws-wrong")[2]["result"]
        self.assertTrue(r["isError"])
        r = self.call("coal_status", workspace_id=self.machine.workspace_id)[2]["result"]
        self.assertFalse(r["isError"])


class Token(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="coal-token-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_first_run_generates_a_0600_file_and_reuses_it(self):
        path = os.path.join(self.tmp, "cfg", "token")
        t = auth.load_or_create_token(path)
        self.assertGreaterEqual(len(t), 32)
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        self.assertEqual(auth.load_or_create_token(path), t)

    def test_a_readable_token_file_is_refused(self):
        path = os.path.join(self.tmp, "token")
        with open(path, "w") as f:
            f.write("x" * 40)
        os.chmod(path, 0o644)
        with self.assertRaises(ToolError):
            auth.load_or_create_token(path)

    def test_short_token_is_refused(self):
        path = os.path.join(self.tmp, "token")
        fd = os.open(path, os.O_WRONLY | os.O_CREAT, 0o600)
        os.write(fd, b"short\n")
        os.close(fd)
        with self.assertRaises(ToolError):
            auth.load_or_create_token(path)

    def test_bearer_check_is_exact(self):
        a = auth.BearerToken("secret-token-value")
        self.assertIsNotNone(a.authenticate({"Authorization": "Bearer secret-token-value"}))
        self.assertIsNotNone(a.authenticate({"Authorization": "bearer secret-token-value"}))
        for bad in ({}, {"Authorization": "Bearer secret-token-valu"}, {"Authorization": "Basic secret-token-value"},
                    {"Authorization": "secret-token-value"}):
            self.assertIsNone(a.authenticate(bad))


if __name__ == "__main__":
    unittest.main()
