"""The MCP method dispatcher shared by the stdio and HTTP transports.

Gateway.handle takes one decoded JSON-RPC message and returns the response to send (None for a
notification). It keeps no per-connection state: everything a call needs arrives in the request,
which is what the stateless 2026-07-28 revision asks for. The initialize handshake of older
revisions is still answered so existing clients keep working.
"""
import threading
import time

from . import tasks as tasks_ext
from . import tools as toolset
from .errors import ToolError
from .guide import GUIDE, GUIDE_URI, INSTRUCTIONS
from .runlog import valid_run_id

MODERN_VERSIONS = ["2026-07-28"]
LEGACY_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"]
META_VERSION = "io.modelcontextprotocol/protocolVersion"
META_CAPS = "io.modelcontextprotocol/clientCapabilities"
META_SERVER = "io.modelcontextprotocol/serverInfo"
METHODS = ("initialize", "server/discover", "ping", "resources/list", "resources/read", "tools/list",
           "tools/call", "tasks/get", "tasks/update", "tasks/cancel")

PARSE_ERROR, INVALID_REQUEST, METHOD_NOT_FOUND, INVALID_PARAMS, INTERNAL_ERROR = -32700, -32600, -32601, -32602, -32603
UNSUPPORTED_VERSION = -32022


class RpcError(Exception):
    def __init__(self, code, message, data=None):
        super().__init__(message)
        self.code, self.message, self.data = code, message, data


def error_response(rid, code, message, data=None):
    e = {"code": code, "message": message}
    if data is not None:
        e["data"] = data
    return {"jsonrpc": "2.0", "id": rid, "error": e}


def tool_result(text, data=None, is_error=False):
    r = {"content": [{"type": "text", "text": text}], "isError": is_error}
    if data is not None:
        r["structuredContent"] = data
    return r


class Gateway:
    def __init__(self, machine, runlog, server_info, log=lambda msg: None):
        self.machine = machine
        self.runlog = runlog
        self.server_info = server_info
        self.log = log
        self.tasks = tasks_ext.TaskStore()
        self.default_run = "run-" + time.strftime("%Y%m%d-%H%M%S") + "-%04x" % (id(self) & 0xFFFF)
        self.caps = {"tools": {"listChanged": False}, "resources": {"listChanged": False},
                     "extensions": {tasks_ext.EXTENSION: {}}}

    # -- entry point ------------------------------------------------------------------

    def handle(self, msg):
        rid, method = msg.get("id"), msg.get("method")
        if not isinstance(method, str):
            return None if ("id" in msg and "method" not in msg) else error_response(rid, INVALID_REQUEST, "Invalid Request")
        if "id" not in msg:
            return None                                          # notifications need no action
        params = msg.get("params") or {}
        if not isinstance(params, dict):
            return error_response(rid, INVALID_PARAMS, "params must be an object")
        meta = params.get("_meta") if isinstance(params.get("_meta"), dict) else {}
        modern = META_VERSION in meta
        if modern and meta[META_VERSION] not in MODERN_VERSIONS:
            return error_response(rid, UNSUPPORTED_VERSION, "Unsupported protocol version",
                                  {"supported": MODERN_VERSIONS + LEGACY_VERSIONS, "requested": meta[META_VERSION]})
        if method not in METHODS:
            return error_response(rid, METHOD_NOT_FOUND, "Method not found: " + method)
        try:
            result = self.dispatch(method, params, meta)
        except RpcError as e:
            return error_response(rid, e.code, e.message, e.data)
        except tasks_ext.UnknownTask as e:
            return error_response(rid, INVALID_PARAMS, "Unknown task: %s" % e)
        except ValueError as e:
            return error_response(rid, INVALID_PARAMS, str(e))
        except Exception as e:                                   # a bug here must not kill the server
            self.log("internal error in %s: %r" % (method, e))
            return error_response(rid, INTERNAL_ERROR, "Internal error: %s" % e)
        if modern:
            result.setdefault("resultType", "complete")
            result.setdefault("_meta", {})[META_SERVER] = self.server_info
        return {"jsonrpc": "2.0", "id": rid, "result": result}

    def dispatch(self, method, params, meta):
        if method == "initialize":
            wanted = params.get("protocolVersion")
            return {"protocolVersion": wanted if wanted in LEGACY_VERSIONS else LEGACY_VERSIONS[0],
                    "capabilities": self.caps, "serverInfo": self.server_info, "instructions": INSTRUCTIONS}
        if method == "server/discover":
            return {"supportedVersions": MODERN_VERSIONS + LEGACY_VERSIONS, "capabilities": self.caps,
                    "serverInfo": self.server_info, "instructions": INSTRUCTIONS,
                    "ttlMs": 3600000, "cacheScope": "public"}
        if method == "ping":
            return {}
        if method == "resources/list":
            return {"resources": [{"uri": GUIDE_URI, "name": "coal-guide", "title": "Coal OS system guide",
                                   "mimeType": "text/plain"}], "ttlMs": 3600000, "cacheScope": "public"}
        if method == "resources/read":
            if params.get("uri") != GUIDE_URI:
                raise ValueError("Unknown resource: %s" % params.get("uri"))
            return {"contents": [{"uri": GUIDE_URI, "mimeType": "text/plain", "text": GUIDE}],
                    "ttlMs": 3600000, "cacheScope": "public"}
        if method == "tools/list":
            return {"tools": toolset.listing(), "ttlMs": 3600000, "cacheScope": "public"}
        if method == "tools/call":
            return self.call_tool(params, meta)
        return self.task_method(method, params)

    # -- tools ------------------------------------------------------------------------

    def call_tool(self, params, meta):
        name = params.get("name")
        if name not in toolset.BY_NAME:
            raise ValueError("Unknown tool: %s" % name)
        args = params.get("arguments") or {}
        if not isinstance(args, dict):
            raise ValueError("arguments must be an object")
        args = dict(args)
        run_id = args.pop("run_id", None) or self.default_run
        workspace_id = args.pop("workspace_id", None)
        if not valid_run_id(run_id):
            raise ValueError("run_id must be 1-64 characters: letters, digits, _ . -, starting with a letter or digit")
        t = toolset.BY_NAME[name]
        started = time.time()

        def execute(cancel, task_id=None):
            return self.run_tool(t, args, run_id, workspace_id, started, cancel, task_id)

        wants_task = tasks_ext.EXTENSION in ((meta.get(META_CAPS) or {}).get("extensions") or {})
        if wants_task and t.runs_as_task(args):
            try:
                created = self.tasks.create(execute)
            except OverflowError as e:
                raise ToolError(str(e))
            created["resultType"] = "task"
            return created
        return execute(threading.Event())

    def run_tool(self, t, args, run_id, workspace_id, started, cancel, task_id):
        """Runs one tool call to a result dict and appends it to the run log. Never raises ToolError."""
        try:
            if workspace_id is not None and workspace_id != self.machine.workspace_id:
                raise ToolError("unknown workspace_id %r; this gateway serves %s" % (workspace_id, self.machine.workspace_id))
            out = t.fn(toolset.Call(self.machine, run_id, cancel), args)
            text, data = out if isinstance(out, tuple) else (out, None)
            result = tool_result(text, data)
        except ToolError as e:
            result = tool_result(str(e), None, True)
        except (OSError, ValueError, KeyError, TypeError) as e:
            result = tool_result("%s failed: %s: %s" % (t.name, type(e).__name__, e), None, True)
        self.record(t, args, run_id, started, result, task_id)
        return result

    def record(self, t, args, run_id, started, result, task_id):
        agent = self.machine.agent
        extra = {"task_id": task_id} if task_id else None
        try:
            self.runlog.record(run_id, t.name, args, result["content"][0]["text"], result["isError"], started,
                               boot=agent.boot_seq if agent else None, extra=extra)
        except OSError as e:                                     # a full disk must not fail the call itself
            self.log("run log write failed: %s" % e)

    # -- tasks ------------------------------------------------------------------------

    def task_method(self, method, params):
        task_id = params.get("taskId")
        if not isinstance(task_id, str):
            raise ValueError("taskId must be a string")
        if method == "tasks/get":
            return self.tasks.get(task_id)
        if method == "tasks/cancel":
            self.tasks.cancel(task_id)
        else:
            self.tasks.get(task_id)                              # tasks/update: only checks the task exists
        return {}
