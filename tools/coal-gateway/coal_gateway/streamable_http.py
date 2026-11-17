"""MCP over Streamable HTTP, stateless (2026-07-28): POST /mcp, one JSON response per request.

No sessions, no SSE, no server-initiated messages. Long calls are tasks that the client polls.
Defences, in order: Origin allowlist (DNS rebinding), bearer authentication, body size cap,
header/body consistency.
"""
import ipaddress
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

from .mcp import INVALID_REQUEST, MODERN_VERSIONS, PARSE_ERROR, error_response

MAX_BODY = 8 * 1024 * 1024
HEADER_MISMATCH = -32020
ENDPOINT = "/mcp"
METADATA_PATH = "/.well-known/oauth-protected-resource"


def is_loopback(host):
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return host == "localhost"


def origin_allowed(origin, extra=()):
    """No Origin header (a non-browser client) is fine; a browser's must be this machine or listed."""
    if origin is None or origin in extra:
        return True
    return is_loopback(urlsplit(origin).hostname or "")


class HttpServer:
    def __init__(self, gateway, authenticator, host="127.0.0.1", port=7077, allow_origins=(),
                 allow_non_loopback=False):
        if not is_loopback(host) and not allow_non_loopback:
            raise ValueError("refusing to listen on %s: only loopback is allowed unless --allow-non-loopback is "
                             "given (and then use TLS in front and a real OAuth 2.1 authenticator)" % host)
        self.gateway, self.auth = gateway, authenticator
        self.allow_origins = tuple(allow_origins)
        outer = self

        class Handler(_Handler):
            server_ctx = outer

        self.httpd = ThreadingHTTPServer((host, port), Handler)
        self.httpd.daemon_threads = True

    @property
    def address(self):
        return self.httpd.server_address[:2]

    def serve(self):
        self.httpd.serve_forever()

    def start(self):
        threading.Thread(target=self.serve, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def _mismatch(headers, msg):
    """A message for -32020 when the standard request headers disagree with the body, else None."""
    method = msg.get("method") if isinstance(msg, dict) else None
    params = msg.get("params") if isinstance(msg, dict) and isinstance(msg.get("params"), dict) else {}
    version = headers.get("MCP-Protocol-Version")
    meta = params.get("_meta") if isinstance(params.get("_meta"), dict) else {}
    body_version = meta.get("io.modelcontextprotocol/protocolVersion")
    if version and body_version and version != body_version:
        return "MCP-Protocol-Version %s does not match _meta %s" % (version, body_version)
    named = {"tools/call": params.get("name"), "resources/read": params.get("uri")}
    name = named.get(method, params.get("taskId") if str(method).startswith("tasks/") else None)
    if headers.get("Mcp-Method") and headers["Mcp-Method"] != method:
        return "Mcp-Method %s does not match the request method %s" % (headers["Mcp-Method"], method)
    if headers.get("Mcp-Name") and name is not None and headers["Mcp-Name"] != name:
        return "Mcp-Name %s does not match %s" % (headers["Mcp-Name"], name)
    if version in MODERN_VERSIONS and not headers.get("Mcp-Method"):
        return "the Mcp-Method header is required"
    return None


class _Handler(BaseHTTPRequestHandler):
    server_ctx = None
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):                  # request lines would put paths in the server's stderr
        pass

    def reply(self, status, body=None, headers=()):
        data = b"" if body is None else json.dumps(body, separators=(",", ":"), ensure_ascii=False).encode()
        self.send_response(status)
        if body is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def reject(self, status, code, message, headers=()):
        self.reply(status, error_response(None, code, message), headers)

    def guard(self):
        """Origin and credentials; replies and returns False when the request must not proceed."""
        ctx = self.server_ctx
        if not origin_allowed(self.headers.get("Origin"), ctx.allow_origins):
            self.reject(403, INVALID_REQUEST, "Origin not allowed")
            return False
        if ctx.auth.authenticate(self.headers) is None:
            self.reject(401, INVALID_REQUEST, "Authentication required",
                        [("WWW-Authenticate", ctx.auth.challenge())])
            return False
        return True

    def do_GET(self):
        meta = self.server_ctx.auth.resource_metadata()
        if self.path == METADATA_PATH and meta is not None:
            return self.reply(200, meta)
        if self.path != ENDPOINT:
            return self.reject(404, INVALID_REQUEST, "Not found")
        if self.guard():
            self.reject(405, INVALID_REQUEST, "Use POST", [("Allow", "POST")])

    do_PUT = do_DELETE = do_PATCH = do_GET

    def do_POST(self):
        if self.path != ENDPOINT:
            return self.reject(404, INVALID_REQUEST, "Not found")
        if not self.guard():
            return
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return self.reject(411, INVALID_REQUEST, "Content-Length required")
        if length < 0 or length > MAX_BODY:
            self.close_connection = True
            return self.reject(413, INVALID_REQUEST, "Body larger than %d bytes" % MAX_BODY)
        raw = self.rfile.read(length)
        try:
            msg = json.loads(raw)
        except ValueError:
            return self.reject(400, PARSE_ERROR, "Parse error")
        if not isinstance(msg, dict):
            return self.reject(400, INVALID_REQUEST, "A single JSON-RPC request object is required")
        problem = _mismatch(self.headers, msg)
        if problem:
            return self.reply(400, error_response(msg.get("id"), HEADER_MISMATCH, problem))
        response = self.server_ctx.gateway.handle(msg)
        if response is None:
            return self.reply(202)
        self.reply(200, response)
