"""MCP over stdio: one JSON-RPC message per line on stdin, one per line on stdout."""
import json
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

from .mcp import INVALID_REQUEST, PARSE_ERROR, error_response


class StdioServer:
    def __init__(self, gateway, stdin=None, stdout=None):
        self.gateway = gateway
        self.stdin = stdin or sys.stdin.buffer
        self.stdout = stdout or sys.stdout
        self.out_lock = threading.Lock()
        self.pool = ThreadPoolExecutor(max_workers=8)

    def send(self, msg):
        data = json.dumps(msg, separators=(",", ":"), ensure_ascii=False) + "\n"
        with self.out_lock:
            self.stdout.write(data)
            self.stdout.flush()

    def handle(self, msg):
        response = self.gateway.handle(msg)
        if response is not None:
            self.send(response)

    def serve(self):
        for raw in self.stdin:
            raw = raw.strip()
            if not raw:
                continue
            try:
                msg = json.loads(raw)
            except ValueError:
                self.send(error_response(None, PARSE_ERROR, "Parse error"))
                continue
            if isinstance(msg, list):
                self.send(error_response(None, INVALID_REQUEST, "Batches are not supported"))
            elif not isinstance(msg, dict):
                self.send(error_response(None, INVALID_REQUEST, "Invalid Request"))
            else:
                self.pool.submit(self.handle, msg)
        self.pool.shutdown(wait=False, cancel_futures=True)
