"""Per-run JSONL log of every tool call: the raw material of a replay bundle.

One file per run_id, one record per call, appended as the call finishes. A record names the call and
fingerprints its result (sha256 and size) instead of copying it, and secrets never reach the file:
payloads are logged as digest and length, environment values and secret-looking keys are masked, and
free text is scrubbed of bearer tokens and token=/password= pairs.
"""
import hashlib
import json
import os
import re
import threading
import time

PAYLOAD_KEYS = {"content", "content_base64", "data", "text"}     # bulk or possibly sensitive: digest only
SECRET_KEY = re.compile(r"token|secret|passw|authorization|api[_-]?key|credential|cookie", re.I)
SCRUBBERS = [
    (re.compile(r"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]+"), "Bearer [redacted]"),
    (re.compile(r"(?i)\b(token|password|passwd|secret|api[_-]?key|access[_-]?key)(\s*[=:]\s*)\S+"), r"\1\2[redacted]"),
    (re.compile(r"(?i)(://[^/\s:@]+:)[^/\s@]+@"), r"\1[redacted]@"),
]
MAX_STRING = 1024
RUN_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}")


def valid_run_id(run_id):
    return isinstance(run_id, str) and RUN_ID.fullmatch(run_id) is not None


def digest(data):
    if isinstance(data, str):
        data = data.encode("utf-8", "replace")
    return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}


def scrub(text):
    for pattern, repl in SCRUBBERS:
        text = pattern.sub(repl, text)
    return text


def redact(value, key=None):
    """A copy of a tool argument that is safe to write down."""
    if key is not None and SECRET_KEY.search(key):
        return "[redacted]"
    if key in PAYLOAD_KEYS and isinstance(value, str):
        return digest(value)
    if key == "env" and isinstance(value, dict):
        return {k: "[redacted]" for k in value}
    if isinstance(value, dict):
        return {k: redact(v, k) for k, v in value.items()}
    if isinstance(value, list):
        return [redact(v) for v in value[:64]]
    if isinstance(value, str):
        value = scrub(value)
        return value if len(value) <= MAX_STRING else value[:MAX_STRING] + "...(%d chars)" % len(value)
    return value


class RunLog:
    def __init__(self, directory, workspace_id):
        self.directory = directory
        self.workspace_id = workspace_id             # callable: the id is minted lazily with the build dir
        self.lock = threading.Lock()
        self.seq = {}

    def path(self, run_id):
        return os.path.join(self.directory, run_id + ".jsonl")

    def _next_seq(self, run_id):
        if run_id not in self.seq:                   # continue a run that an earlier process started
            try:
                with open(self.path(run_id), "rb") as f:
                    self.seq[run_id] = sum(1 for _ in f)
            except OSError:
                self.seq[run_id] = 0
        self.seq[run_id] += 1
        return self.seq[run_id]

    def record(self, run_id, tool, args, text, is_error, started, boot=None, extra=None):
        """Appends one call. `started` is time.time() at the start; the duration is measured now."""
        entry = {"run_id": run_id, "ts": round(time.time(), 3), "workspace_id": self.workspace_id(),
                 "tool": tool, "args": redact(args), "is_error": bool(is_error),
                 "result": digest(text), "started_at": round(started, 3),
                 "duration_ms": int((time.time() - started) * 1000), "boot": boot}
        if extra:
            entry.update(extra)
        with self.lock:
            entry["seq"] = self._next_seq(run_id)
            os.makedirs(self.directory, mode=0o700, exist_ok=True)
            fd = os.open(self.path(run_id), os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
            with os.fdopen(fd, "a", encoding="utf-8") as f:
                f.write(json.dumps(entry, separators=(",", ":"), ensure_ascii=False) + "\n")
        return entry
