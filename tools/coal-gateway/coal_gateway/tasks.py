"""The MCP tasks extension (io.modelcontextprotocol/tasks): long tool calls as pollable handles.

The server decides per call whether to answer with a task. A task runs the call on its own thread;
tasks/get reports the state and, once finished, the result or error; tasks/cancel is cooperative: it
sets the Event the call watches, and the call decides how fast to stop. tasks/update has nothing to
deliver (no call asks the client for input) and is acknowledged.
"""
import threading
import time
import uuid
from datetime import datetime, timezone

EXTENSION = "io.modelcontextprotocol/tasks"
TTL_MS = 3600 * 1000
POLL_MS = 1000
MAX_TASKS = 256


def _iso(t):
    return datetime.fromtimestamp(t, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class UnknownTask(KeyError):
    pass


class Task:
    def __init__(self, work):
        self.id = str(uuid.uuid4())
        self.created = self.updated = time.time()
        self.status = "working"
        self.message = "The operation is in progress."
        self.result = self.error = None
        self.cancel = threading.Event()
        self.work = work

    def view(self, ttl_ms):
        v = {"taskId": self.id, "status": self.status, "statusMessage": self.message,
             "createdAt": _iso(self.created), "lastUpdatedAt": _iso(self.updated),
             "ttlMs": ttl_ms, "pollIntervalMs": POLL_MS}
        if self.status == "completed":
            v["result"] = self.result
        elif self.status == "failed":
            v["error"] = self.error
        return v


class TaskStore:
    def __init__(self, ttl_ms=TTL_MS):
        self.ttl_ms = ttl_ms
        self.lock = threading.Lock()
        self.tasks = {}

    def create(self, work):
        """Starts work(cancel_event, task_id) -> result dict on a thread; returns the new task's view.

        An exception escaping work makes the task `failed` with a JSON-RPC error.
        """
        task = Task(work)
        with self.lock:
            self._expire()
            if len(self.tasks) >= MAX_TASKS:
                raise OverflowError("too many tasks")
            self.tasks[task.id] = task
        threading.Thread(target=self._run, args=(task,), daemon=True).start()
        return task.view(self.ttl_ms)

    def _run(self, task):
        try:
            result = task.work(task.cancel, task.id)
            outcome = ("completed", result, None)
        except Exception as e:                                   # a bug in a tool must not strand the task
            outcome = ("failed", None, {"code": -32603, "message": "Internal error: %s" % e})
        with self.lock:
            task.status, task.result, task.error = outcome
            if task.cancel.is_set() and task.status == "completed":
                task.status, task.result = "cancelled", None
            task.message = {"completed": "Done.", "failed": "The operation failed.",
                            "cancelled": "Cancelled."}[task.status]
            task.updated = time.time()

    def _get(self, task_id):
        task = self.tasks.get(task_id)
        if task is None:
            raise UnknownTask(task_id)
        return task

    def get(self, task_id):
        with self.lock:
            self._expire()
            return self._get(task_id).view(self.ttl_ms)

    def cancel(self, task_id):
        with self.lock:
            self._get(task_id).cancel.set()

    def _expire(self):
        """Forgets finished tasks past their ttl (caller holds the lock)."""
        now = time.time()
        for tid in [t.id for t in self.tasks.values()
                    if t.status != "working" and now > t.updated + self.ttl_ms / 1000.0]:
            del self.tasks[tid]
