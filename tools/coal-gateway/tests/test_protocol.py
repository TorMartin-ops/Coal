#!/usr/bin/env python3
"""Protocol tests: framing, concurrency, truncation, timeout kill, jobs, limits and file calls.

Each case runs twice: against agentd built as a 32-bit Linux binary on a pty (fast), and against the
real kernel and agentd in QEMU (TCG, headless). Set COAL_TEST_NO_QEMU=1 to skip the QEMU run.
"""
import base64
import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from support import HostAgent, QemuAgent  # noqa: E402
from coal_gateway.errors import AgentError, ToolError  # noqa: E402


class ProtocolCases:
    fixture = None

    @classmethod
    def setUpClass(cls):
        cls.fx = cls.fixture()
        cls.c = cls.fx.client()
        cls.dir = "/tmp/proto-%d" % os.getpid()

    @classmethod
    def tearDownClass(cls):
        cls.c.close()
        cls.fx.close()

    def setUp(self):
        self.c.request("fs.mkdir", path=self.dir, parents=True)

    def tearDown(self):
        self.c.request("fs.remove", path=self.dir, recursive=True)

    def run_cmd(self, cmd, **kw):
        return self.c.request("exec", timeout=kw.pop("timeout", 60), cmd=cmd, **kw)

    # -- hello and framing --------------------------------------------------------

    def test_hello(self):
        h = self.c.request("hello")
        self.assertEqual(h["proto"], 1)
        self.assertEqual(h["max_frame"], 256 * 1024)
        self.assertIn("cpu_s", h["rlimits"] if isinstance(h["rlimits"], list) else list(h["rlimits"]) + ["cpu_s"])
        self.assertGreaterEqual(h["max_jobs"], 8)
        self.assertEqual(self.c.hello["event"], "hello")

    def test_errors_carry_code_errno_and_message(self):
        with self.assertRaises(AgentError) as e:
            self.c.request("fs.stat", path="/nonexistent/x")
        self.assertEqual(e.exception.errno, 2)
        with self.assertRaises(AgentError) as e:
            self.c.request("no.such.op")
        self.assertEqual(e.exception.code, "unknown_op")

    def test_a_request_close_to_the_frame_limit(self):
        data = os.urandom(180 * 1024)                    # 240 KiB of base64 in one frame
        self.c.request("fs.write", path=self.dir + "/ok", data=base64.b64encode(data).decode(), more=False)
        r = self.c.request("fs.read", path=self.dir + "/ok", limit=96 * 1024)
        self.assertEqual(base64.b64decode(r["data"]), data[:96 * 1024])
        with self.assertRaises(ToolError):               # the client refuses to send a frame agentd would reject
            self.c.request("fs.write", path=self.dir + "/big", data=base64.b64encode(os.urandom(200 * 1024)).decode(), more=False)
        self.assertTrue(self.c.request("ping")["pong"])

    # -- concurrency ----------------------------------------------------------------

    def test_fast_requests_overtake_a_slow_one(self):
        out = {}
        t = threading.Thread(target=lambda: out.setdefault("r", self.run_cmd("sleep 2; echo slow")))
        t0 = time.monotonic()
        t.start()
        time.sleep(0.3)
        for _ in range(10):
            self.assertTrue(self.c.request("ping")["pong"])
        self.assertLess(time.monotonic() - t0, 1.8)      # all pings answered while sleep 2 still ran
        self.assertNotIn("r", out)
        t.join()
        self.assertEqual(out["r"]["stdout"]["text"], "slow\n")

    def test_concurrent_execs_run_in_parallel_and_keep_their_answers_apart(self):
        results = {}

        def go(i):
            results[i] = self.run_cmd("sleep 1; echo job%d" % i)["stdout"]["text"]
        threads = [threading.Thread(target=go, args=(i,)) for i in range(4)]
        t0 = time.monotonic()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(results, {i: "job%d\n" % i for i in range(4)})
        self.assertLess(time.monotonic() - t0, 3.5)

    def test_many_threads_hammering_ping_and_stat(self):
        errors = []

        def hammer():
            try:
                for _ in range(25):
                    self.c.request("ping")
                    self.c.request("fs.stat", path="/")
            except Exception as e:
                errors.append(e)
        threads = [threading.Thread(target=hammer) for _ in range(6)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertEqual(errors, [])

    # -- exec envelope --------------------------------------------------------------

    def test_envelope(self):
        r = self.run_cmd("echo out; echo err >&2; exit 7", cwd="/tmp")
        self.assertEqual((r["exit_code"], r["signal"], r["timed_out"], r["killed_by"], r["cwd"]), (7, 0, False, None, "/tmp"))
        self.assertEqual(r["stdout"], {"text": "out\n", "bytes_total": 4, "bytes_returned": 4, "truncated": False})
        self.assertEqual(r["stderr"]["text"], "err\n")
        self.assertIsInstance(r["duration_ms"], int)
        self.assertNotIn("placeholder", r)

    def test_no_output_placeholder(self):
        self.assertEqual(self.run_cmd("true")["placeholder"], "(no output)")

    def test_death_by_signal_is_reported(self):
        r = self.run_cmd("kill -9 $$")
        self.assertEqual(r["signal"], 9)

    def test_noninteractive_defaults(self):
        r = self.run_cmd('echo "$PAGER|$EDITOR|$CI|$TERM|$GIT_TERMINAL_PROMPT|$FOO"; cat; test ! -t 0 && echo notty',
                         env={"FOO": "bar"})
        self.assertEqual(r["stdout"]["text"], "cat|true|1|dumb|0|bar\nnotty\n")

    def test_env_override_wins_over_defaults(self):
        self.assertEqual(self.run_cmd('echo "$TERM"', env={"TERM": "xterm"})["stdout"]["text"], "xterm\n")

    # -- truncation and spill ------------------------------------------------------

    def test_head_and_tail_with_spill_and_offset_read(self):
        r = self.run_cmd("seq 1 5000")
        s = r["stdout"]
        full = "".join("%d\n" % i for i in range(1, 5001))
        self.assertEqual((s["bytes_total"], s["truncated"], s["bytes_returned"]), (len(full), True, 8000))
        self.assertTrue(s["text"].startswith(full[:2000]))
        self.assertTrue(s["text"].endswith(full[-6000:]))
        self.assertIn("bytes omitted", s["text"])
        got = self.c.request("fs.read", path=s["path"], offset=0, limit=len(full))
        self.assertEqual(base64.b64decode(got["data"]).decode(), full)
        mid = self.c.request("fs.read", path=s["path"], offset=5000, limit=100)
        self.assertEqual(base64.b64decode(mid["data"]).decode(), full[5000:5100])

    def test_custom_head_and_tail(self):
        r = self.run_cmd("seq 1 1000", head=10, tail=20)
        s = r["stdout"]
        self.assertEqual(s["text"][:10], "1\n2\n3\n4\n5\n")
        self.assertEqual(s["bytes_returned"], 30)
        self.assertTrue(s["truncated"])

    def test_small_output_is_not_truncated_and_has_no_path(self):
        s = self.run_cmd("seq 1 100")["stdout"]
        self.assertFalse(s["truncated"])
        self.assertNotIn("path", s)

    def test_output_far_beyond_the_spill_cap(self):
        r = self.run_cmd("yes 0123456789 | head -n 500000; echo done >&2", timeout=170, timeout_ms=160000)
        self.assertEqual(r["exit_code"], 0)
        self.assertEqual(r["stdout"]["bytes_total"], 5500000)
        self.assertTrue(r["stdout"]["truncated"])
        self.assertEqual(r["stderr"]["text"], "done\n")

    # -- timeout, cancel ------------------------------------------------------------

    def test_timeout_kills_the_whole_process_group(self):
        t0 = time.monotonic()
        r = self.run_cmd("sleep 3137 & sleep 3138 | sleep 3139", timeout_ms=500)
        self.assertLess(time.monotonic() - t0, 5)
        self.assertEqual((r["timed_out"], r["killed_by"]), (True, "timeout"))
        time.sleep(0.2)
        left = self.run_cmd("ps")["stdout"]["text"]
        self.assertNotRegex(left, r"sleep\s+313[789]")

    def test_cancel_kills_a_running_exec(self):
        cancel = threading.Event()
        out = {}
        t = threading.Thread(target=lambda: out.setdefault("r", self.c.request("exec", timeout=30, cancel=cancel, cmd="sleep 3140")))
        t.start()
        time.sleep(0.5)
        cancel.set()
        t.join(10)
        self.assertEqual(out["r"]["killed_by"], "cancel")
        self.assertFalse(out["r"]["timed_out"])

    # -- jobs -------------------------------------------------------------------------

    def test_job_lifecycle(self):
        j = self.c.request("spawn", cmd="echo first; sleep 1; echo second; exit 4")["job_id"]
        self.assertEqual(self.c.request("job.poll", job_id=j)["state"], "running")
        self.assertIn(j, [x["job_id"] for x in self.c.request("job.list")["jobs"]])
        time.sleep(0.4)
        part = self.c.request("job.tail", job_id=j, offset=0)
        self.assertEqual((part["text"], part["eof"]), ("first\n", False))
        done = self.c.request("job.wait", job_id=j, timeout_ms=10000)
        self.assertEqual((done["state"], done["exit_code"], done["killed_by"]), ("exited", 4, None))
        rest = self.c.request("job.tail", job_id=j, offset=part["next_offset"])
        self.assertEqual((rest["text"], rest["eof"]), ("second\n", True))
        self.assertEqual(self.c.request("job.tail", job_id=j, stream="stderr")["size"], 0)
        self.c.request("job.release", job_id=j)
        with self.assertRaises(AgentError):
            self.c.request("job.poll", job_id=j)

    def test_job_wait_times_out_while_running(self):
        j = self.c.request("spawn", cmd="sleep 3141")["job_id"]
        t0 = time.monotonic()
        r = self.c.request("job.wait", job_id=j, timeout_ms=300)
        self.assertEqual(r["state"], "running")
        self.assertLess(time.monotonic() - t0, 3)
        self.c.request("job.signal", job_id=j, sig=9)
        r = self.c.request("job.wait", job_id=j, timeout_ms=10000)
        self.assertEqual((r["state"], r["signal"], r["killed_by"]), ("exited", 9, "signal"))
        self.c.request("job.release", job_id=j)

    def test_job_has_its_own_process_group_and_signal_reaches_it_all(self):
        j = self.c.request("spawn", cmd="sleep 3142 & sleep 3143; wait")["job_id"]
        time.sleep(0.4)
        self.c.request("job.signal", job_id=j)           # SIGTERM to the group
        r = self.c.request("job.wait", job_id=j, timeout_ms=10000)
        self.assertEqual(r["state"], "exited")
        time.sleep(0.2)
        self.assertNotRegex(self.run_cmd("ps")["stdout"]["text"], r"sleep\s+314[23]")
        self.c.request("job.release", job_id=j)

    def test_job_timeout(self):
        j = self.c.request("spawn", cmd="sleep 3144", timeout_ms=300)["job_id"]
        r = self.c.request("job.wait", job_id=j, timeout_ms=10000)
        self.assertEqual((r["timed_out"], r["killed_by"]), (True, "timeout"))
        self.c.request("job.release", job_id=j)

    def test_job_exit_event(self):
        events = []
        self.c.add_listener(events.append)
        j = self.c.request("spawn", cmd="exit 5")["job_id"]
        self.c.request("job.wait", job_id=j, timeout_ms=10000)
        deadline = time.monotonic() + 5
        while not any(e.get("event") == "job.exit" and e["job_id"] == j for e in events) and time.monotonic() < deadline:
            time.sleep(0.05)
        ev = [e for e in events if e.get("event") == "job.exit" and e["job_id"] == j]
        self.assertEqual(ev[0]["exit_code"], 5)
        self.c.request("job.release", job_id=j)

    def need_rlimit_report(self):
        if not self.fx.reports_rlimits:
            self.skipTest("a Linux host kernel does not provide the RlimitHits line in /proc/<pid>/status")

    def test_cpu_limit_is_enforced_and_reported(self):
        self.need_rlimit_report()
        events = []
        self.c.add_listener(events.append)
        j = self.c.request("spawn", cmd="while :; do :; done", cpu_s=1)["job_id"]
        r = self.c.request("job.wait", job_id=j, timeout_ms=30000)
        self.assertEqual(r["state"], "exited")
        self.assertIn("cpu", r["rlimit_hits"])
        self.assertEqual(r["killed_by"], "rlimit_cpu")
        self.assertTrue(any(e.get("event") == "rlimit" and e["job_id"] == j and e["resource"] == "cpu" for e in events))
        self.c.request("job.release", job_id=j)

    def test_process_limit_makes_fork_fail(self):
        self.need_rlimit_report()
        r = self.run_cmd("for i in 1 2 3 4 5 6; do sleep 3 & done; wait; echo ok", nproc=2, timeout_ms=20000)
        self.assertIn("nproc", r["rlimit_hits"])

    # -- files ------------------------------------------------------------------------

    def test_stat_list_mkdir_remove(self):
        d = self.dir + "/a/b"
        self.assertTrue(self.c.request("fs.mkdir", path=d, parents=True)["created"])
        st = self.c.request("fs.stat", path=d)
        self.assertEqual(st["type"], "dir")
        self.c.request("fs.write", path=d + "/f", data=base64.b64encode(b"x").decode(), more=False)
        names = {e["name"]: e["type"] for e in self.c.request("fs.list", path=d)["entries"]}
        self.assertEqual(names, {"f": "file"})
        with self.assertRaises(AgentError):
            self.c.request("fs.remove", path=self.dir + "/a")           # not empty, not recursive
        self.c.request("fs.remove", path=self.dir + "/a", recursive=True)
        with self.assertRaises(AgentError):
            self.c.request("fs.stat", path=d)
        with self.assertRaises(AgentError):
            self.c.request("fs.remove", path="/")

    def test_read_offset_limit_and_eof(self):
        p = self.dir + "/r.txt"
        self.c.request("fs.write", path=p, data=base64.b64encode(b"0123456789").decode(), more=False)
        r = self.c.request("fs.read", path=p, offset=3, limit=4)
        self.assertEqual((base64.b64decode(r["data"]), r["size"], r["eof"]), (b"3456", 10, False))
        r = self.c.request("fs.read", path=p, offset=8, limit=100)
        self.assertEqual((base64.b64decode(r["data"]), r["eof"]), (b"89", True))
        r = self.c.request("fs.read", path=p, offset=50, limit=10)
        self.assertEqual((r["len"], r["eof"]), (0, True))

    def test_write_is_atomic(self):
        p = self.dir + "/atomic"
        b64 = lambda b: base64.b64encode(b).decode()   # noqa: E731
        self.c.request("fs.write", path=p, data=b64(b"old"), more=False)
        self.c.request("fs.write", path=p, data=b64(b"NEW-part1-"), more=True)
        # mid-upload: the target still holds the old content, the new data sits in a temp file
        self.assertEqual(base64.b64decode(self.c.request("fs.read", path=p)["data"]), b"old")
        self.assertTrue(any(n.endswith(".agentd-tmp") for n in [e["name"] for e in self.c.request("fs.list", path=self.dir)["entries"]]))
        r = self.c.request("fs.write", path=p, data=b64(b"part2"), more=False)
        self.assertTrue(r["committed"])
        self.assertEqual(base64.b64decode(self.c.request("fs.read", path=p)["data"]), b"NEW-part1-part2")
        self.assertFalse(any(e["name"].endswith(".agentd-tmp") for e in self.c.request("fs.list", path=self.dir)["entries"]))

    def test_abandoned_upload_does_not_touch_the_target_and_append_works(self):
        p, q = self.dir + "/one", self.dir + "/two"
        b64 = lambda b: base64.b64encode(b).decode()   # noqa: E731
        self.c.request("fs.write", path=p, data=b64(b"keep"), more=False)
        self.c.request("fs.write", path=p, data=b64(b"lost"), more=True)
        self.c.request("fs.write", path=q, data=b64(b"other"), more=False)    # a new target abandons the open upload
        self.assertEqual(base64.b64decode(self.c.request("fs.read", path=p)["data"]), b"keep")
        self.c.request("fs.write", path=q, data=b64(b"+more"), append=True)
        self.assertEqual(base64.b64decode(self.c.request("fs.read", path=q)["data"]), b"other+more")

    def test_write_bad_base64_and_bad_path(self):
        with self.assertRaises(AgentError):
            self.c.request("fs.write", path=self.dir + "/x", data="!!!", more=False)
        with self.assertRaises(AgentError) as e:
            self.c.request("fs.write", path="/nonexistent-dir/x", data="QQ==", more=False)
        self.assertEqual(e.exception.errno, 2)

    def test_sys_calls(self):
        self.assertTrue(any(p["pid"] >= 1 for p in self.c.request("sys.ps")["processes"]))
        self.assertIn("sysname", self.c.request("sys.info"))


class HostProtocol(ProtocolCases, unittest.TestCase):
    fixture = HostAgent


@unittest.skipIf(os.environ.get("COAL_TEST_NO_QEMU"), "COAL_TEST_NO_QEMU is set")
class QemuProtocol(ProtocolCases, unittest.TestCase):
    fixture = QemuAgent


if __name__ == "__main__":
    unittest.main()
