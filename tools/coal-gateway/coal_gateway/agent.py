"""Client for the agent protocol: multiplexed requests over one Transport."""
import threading
import time

from .errors import AgentError, ToolError
from .frames import Decoder, FrameError, encode


class _Pending:
    __slots__ = ("done", "response")

    def __init__(self):
        self.done = threading.Event()
        self.response = None


class AgentClient:
    """Sends requests with fresh ids and hands each response to its caller, whatever the order.

    Any number of threads may call request() at the same time; agentd answers slow requests (exec,
    job.wait) when they finish and the fast ones in between. Unsolicited events (hello, job.exit,
    rlimit, link_error) go to the listeners.
    """

    def __init__(self, transport):
        self.transport = transport
        self.lock = threading.Lock()           # guards the fields below and serialises frame writes
        self.connected = False
        self.next_id = 1
        self.pending = {}
        self.listeners = []
        self.hello = {}                        # last hello event
        self.boot_seq = 0                      # counts hello events: job ids are only valid within one
        self.end_reason = ""                   # why the last connection ended
        self.explain = None                    # optional callable: more about the other end (QEMU's fate)
        self.cond = threading.Condition(self.lock)

    # -- connection ---------------------------------------------------------

    def connect(self, timeout=5.0):
        with self.lock:
            if self.connected:
                return
        self.transport.connect(timeout)
        with self.lock:
            self.connected = True
        threading.Thread(target=self._reader, daemon=True).start()

    def close(self):
        with self.lock:
            was, self.connected = self.connected, False
        if was:
            self.transport.close()
        self._fail_pending()

    def _fail_pending(self):
        with self.lock:
            waiting = list(self.pending.values())
            self.pending.clear()
        for p in waiting:
            p.done.set()                       # response stays None: "connection lost"

    def _reader(self):
        decoder = Decoder()
        reason = "end of stream: the other end closed the connection"
        try:
            while True:
                data = self.transport.recv()
                if not data:
                    break
                for msg in decoder.feed(data):
                    self._dispatch(msg)
        except FrameError as e:
            reason = "bad frame from agentd: %s" % e
        except OSError as e:
            reason = "read failed: %s" % e
        except AttributeError:
            reason = "connection closed locally"
        with self.lock:
            self.end_reason = reason
            self.connected = False
        self.transport.close()
        self._fail_pending()
        with self.cond:
            self.cond.notify_all()

    def _dispatch(self, msg):
        if "event" in msg:
            with self.cond:
                if msg["event"] == "hello":
                    self.hello = msg
                    self.boot_seq += 1
                    self.cond.notify_all()
                listeners = list(self.listeners)
            for fn in listeners:
                try:
                    fn(msg)
                except Exception:              # a listener must not kill the reader
                    pass
            return
        with self.lock:
            p = self.pending.pop(msg.get("id"), None)
        if p:
            p.response = msg
            p.done.set()

    def add_listener(self, fn):
        with self.lock:
            self.listeners.append(fn)

    # -- requests -----------------------------------------------------------

    def lost(self):
        """The message for a request that was cut off: why the link ended and what became of the guest."""
        text = "connection to Coal was lost: %s" % (self.end_reason or "closed locally")
        try:
            extra = self.explain() if self.explain else ""
        except Exception as e:                  # noqa: BLE001 - diagnostics must not hide the error
            extra = "(no diagnosis: %s)" % e
        return text + ("\n" + extra if extra else "")

    def _send(self, op, fields):
        with self.lock:
            rid = self.next_id
            self.next_id += 1
            p = self.pending[rid] = _Pending()
            data = encode(dict(id=rid, op=op, **fields), self.hello.get("max_frame", 256 * 1024))
            try:
                self.transport.send(data)
            except (OSError, AttributeError):
                self.pending.pop(rid, None)
                raise
        return rid, p

    def request(self, op, timeout=15.0, cancel=None, **fields):
        """Sends one request and returns the response fields; raises ToolError / AgentError.

        If the threading.Event `cancel` is set while waiting, agentd is asked to cancel the request
        (exec only) and the call keeps waiting for its answer.
        """
        try:
            self.connect()
        except OSError as e:
            raise ToolError("cannot reach Coal's COM2 (%s). Is it booted?" % e)
        try:
            rid, p = self._send(op, fields)
        except FrameError as e:
            raise ToolError(str(e))
        except (OSError, AttributeError):
            self.close()
            raise ToolError(self.lost() + "\nretry or call coal_boot")
        deadline = time.monotonic() + timeout
        cancelled = False
        while not p.done.wait(0.05 if cancel is not None else max(0.0, deadline - time.monotonic())):
            if cancel is not None and cancel.is_set() and not cancelled:
                cancelled = True
                deadline = min(deadline, time.monotonic() + 10)
                try:
                    self._send("cancel", {"target": rid})
                except (OSError, FrameError):
                    pass
            if time.monotonic() >= deadline:
                break
        if not p.done.is_set():
            with self.lock:
                self.pending.pop(rid, None)
            extra = self.explain() if self.explain else ""
            raise ToolError("no answer from agentd within %.0fs (is it running? see coal_kernel_log)%s"
                            % (timeout, "\n" + extra if extra else ""))
        resp = p.response
        if resp is None:
            raise ToolError(self.lost())
        if not resp.get("ok"):
            raise AgentError(resp.get("error"))
        return resp

    def wait_ready(self, timeout, alive=lambda: True):
        """Pings until agentd answers; returns True on success."""
        end = time.monotonic() + timeout
        while time.monotonic() < end and alive():
            try:
                self.request("ping", timeout=0.3)
                if not self.hello:
                    self.request("hello", timeout=2.0)
                return True
            except ToolError:
                time.sleep(0.05)
        return False
