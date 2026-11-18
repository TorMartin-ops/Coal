#!/usr/bin/env python3
"""Throws malformed and hostile frames at the real agentd (host build on a pty).
Run: python3 tools/coal-mcp/tests/test_agentd_fuzz.py

After every request agentd must still answer a ping, and every frame it sends must be valid
UTF-8 JSON. A hang (no answer for 10 s) or a crash fails the test.
"""
import json
import os
import pty
import random
import select
import shutil
import struct
import subprocess
import tempfile
import time
import tty
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
AGENTD_BUILD = os.path.join(HERE, "..", "..", "..", "src", "CoalOS", "userspace", "agentd", "test", "build_host.sh")
MAX_FRAME = 256 * 1024


def frame(payload):
    return struct.pack("<I", len(payload)) + payload


PING = frame(b'{"id":"sync","op":"ping"}')


@unittest.skipUnless(shutil.which("gcc"), "gcc not available")
class AgentdFuzz(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="coal-fuzz-")
        binary = os.path.join(cls.tmp, "agentd")
        if subprocess.call(["sh", AGENTD_BUILD, binary], stderr=subprocess.DEVNULL) != 0:
            raise unittest.SkipTest("cannot build the host agentd on this host")
        cls.master, slave = pty.openpty()
        tty.setraw(cls.master)
        cls.proc = subprocess.Popen([binary, os.ttyname(slave), os.path.join(cls.tmp, "spill")])
        cls.buf = b""
        cls.read_frame(cls)                                   # the hello event

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        os.close(cls.master)
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def read_frame(self, timeout=10):
        deadline = time.time() + timeout
        while len(self.buf) < 4 or len(self.buf) < 4 + struct.unpack("<I", self.buf[:4])[0]:
            left = deadline - time.time()
            if left <= 0 or not select.select([self.master], [], [], left)[0]:
                raise AssertionError("agentd did not answer within %d s" % timeout)
            self.buf += os.read(self.master, 65536)
        n = struct.unpack("<I", self.buf[:4])[0]
        payload, self.buf = self.buf[4:4 + n], self.buf[4 + n:]
        return payload

    def event(self, name):
        """The next `name` event (job.exit and friends from earlier tests may come first)."""
        while True:
            msg = json.loads(self.read_frame().decode("utf-8"))
            if msg.get("event") == name:
                return msg

    def send(self, data):
        view = memoryview(data)
        while view:                                           # read replies while writing: the pty buffer is small
            r, w, _ = select.select([self.master], [self.master], [], 10)
            if not r and not w:
                raise AssertionError("agentd stopped reading")
            if r:
                self.buf += os.read(self.master, 65536)
            if w:
                view = view[os.write(self.master, view[:4096]):]

    def collect(self):
        """Frames up to the answer to the trailing ping, parsed strictly (bad UTF-8 or JSON fails here)."""
        out = []
        while True:
            msg = json.loads(self.read_frame().decode("utf-8"))
            if msg.get("id") == "sync":
                self.assertEqual(msg, {"id": "sync", "ok": True, "pong": True})
                return out
            out.append(msg)

    def feed(self, payload):
        """Sends payload as one frame, then a ping; returns the replies that came before the ping's."""
        self.assertIsNone(self.proc.poll(), "agentd crashed earlier")
        self.send(frame(payload) + PING)
        replies = self.collect()
        self.assertIsNone(self.proc.poll(), "agentd crashed")
        return replies

    def rejected(self, payload):
        replies = self.feed(payload)
        self.assertEqual(len(replies), 1, replies)
        self.assertFalse(replies[0]["ok"], replies[0])
        return replies[0]

    def test_malformed(self):
        for case in [b"{", b"}", b"[]", b"null", b'"', b"{{{{", b'{"op"', b'{"op":', b'{"op":"ping"',
                     b'{"op":"ping",}', b'{"op" "ping"}', b"{'op':'ping'}", b'{"op":"ping"} trailing',
                     b'{"op":tru}', b'{,}', b'{"":""}', b'{"op":"ping","op":"nope"}', b"\x00", b"{\x00}",
                     b'{"op":"pi\x00ng"}', b" ", b"\r", b'{"op":"exec"}', b'{"op":"nonsense"}', b'{"op":5}',
                     b'{"op":null}', b'{"op":["ping"]}', b'{"op":{"op":"ping"}}']:
            with self.subTest(case=case):
                self.feed(case)

    def test_unknown_and_missing_op_are_errors(self):
        self.assertEqual(self.rejected(b'{"id":1,"op":"nonsense"}')["error"]["code"], "unknown_op")
        self.assertEqual(self.rejected(b'{"id":1}')["error"]["code"], "bad_request")

    def test_ids_are_echoed_as_valid_json(self):
        for raw in [b'"a b"', b"12", b"-3.5", b"true", b"null", b'"q\\"uote"', b'"\\u00e9"', b"ab\"cd", b"1\t",
                    b"1e999", b"[1,2]", b'{"x":1}', b'"' + b"x" * 100 + b'"', b"\x01\x02", b"\xff\xfe"]:
            with self.subTest(id=raw):
                for r in self.feed(b'{"id":' + raw + b',"op":"nonsense"}'):
                    self.assertFalse(r["ok"])

    def test_deep_nesting(self):
        for depth in (7, 8, 9, 100, 5000, 40000):
            for pair in (b"[]", b"{}"):
                nested = b"".join(b"[" if pair == b"[]" else b'{"a":' for _ in range(depth))
                nested += b"0" + b"".join(b"]" if pair == b"[]" else b"}" for _ in range(depth))
                with self.subTest(depth=depth, kind=pair):
                    self.feed(b'{"id":1,"op":"ping","x":' + nested + b"}")
                    self.feed(b'{"id":1,"op":"ping","x":' + nested[:len(nested) // 2] + b"}")

    def resync(self):
        """After a framing error agentd discards input until the line has been quiet for 50 ms."""
        time.sleep(0.2)
        self.send(PING)
        self.collect_after_error()

    def collect_after_error(self):
        while True:
            msg = json.loads(self.read_frame().decode("utf-8"))
            if msg.get("id") == "sync":
                return

    def test_oversized_frame_is_a_link_error_and_resyncs(self):
        self.send(struct.pack("<I", MAX_FRAME + 1) + b"a" * 5000)
        self.event("link_error")
        self.resync()
        self.send(struct.pack("<I", 0xFFFFFFFF))                   # absurd length
        self.event("link_error")
        self.resync()
        self.assertEqual(self.feed(b'{"op":"ping"}')[0]["ok"], True)

    def test_frames_at_the_size_limit(self):
        for size in (MAX_FRAME - 2, MAX_FRAME - 1, MAX_FRAME):
            with self.subTest(size=size):
                self.feed(b"x" * size)
        self.assertIsNone(self.proc.poll())

    def test_empty_and_garbage_frames(self):
        for payload in (b"", b"\x00", b"\xff" * 100, b"GET / HTTP/1.1\r\n\r\n"):
            with self.subTest(payload=payload):
                self.feed(payload)

    def test_frame_split_into_single_bytes_and_glued_frames(self):
        data = frame(b'{"id":"a","op":"ping"}')
        for i in range(len(data)):
            self.send(data[i:i + 1])
            time.sleep(0.002)
        self.assertEqual(json.loads(self.read_frame())["id"], "a")
        self.send(frame(b'{"id":"b","op":"ping"}') + frame(b'{"id":"c","op":"ping"}') + PING)
        self.assertEqual([m["id"] for m in self.collect()], ["b", "c"])

    def test_stalled_partial_frame_is_dropped(self):
        self.send(struct.pack("<I", 100) + b"{")                   # promises 100 bytes, delivers 1
        time.sleep(1.4)
        self.assertIn("incomplete", self.event("link_error")["message"])
        self.resync()

    def test_many_small_requests(self):
        self.send(b"".join(frame(b'{"id":%d,"op":"ping"}' % i) for i in range(500)) + PING)
        self.assertEqual(len(self.collect()), 500)

    def test_invalid_utf8(self):
        for case in [b'{"op":"fs.stat","path":"/\xff\xfe"}', b'{"op":"fs.list","path":"\xc0\xaf"}',
                     b'{"op":"nonsense\xed\xa0\x80"}', b'{"op":"exec","cmd":"echo \xf8\x88\x80\x80\x80"}',
                     b'{"op":"fs.stat","path":"\x80\x80\x80"}', b'{"id":"\xff","op":"ping"}']:
            with self.subTest(case=case):
                replies = self.feed(case)
                if b'"op":"exec"' in case and not any("job_id" in r for r in replies):
                    # exec answers when the command ends, which can be after the trailing ping's pong
                    late = json.loads(self.read_frame().decode("utf-8"))
                    self.assertIn("job_id", late)

    def test_unicode_escapes(self):
        for case in [b'{"op":"fs.stat","path":"\\u"}', b'{"op":"fs.stat","path":"\\u12"}', b'{"op":"fs.stat","path":"\\uZZZZ"}',
                     b'{"op":"fs.stat","path":"\\ud800"}', b'{"op":"fs.stat","path":"\\udc00"}',
                     b'{"op":"fs.stat","path":"\\ud800\\u0041"}', b'{"op":"fs.stat","path":"\\ud83d\\ude00"}',
                     b'{"op":"fs.stat","path":"\\u0000"}', b'{"op":"fs.stat","path":"\\u0000x"}',
                     b'{"op":"fs.stat","path":"\\q"}', b'{"op":"fs.stat","path":"\\', b'{"op":"fs.stat","path":"\\"}',
                     b'{"op":"fs.stat","path":"' + b"\\u00e9" * 20000 + b'"}', b'{"\\u006fp":"ping"}']:
            with self.subTest(case=case[:60]):
                self.feed(case)
        self.assertTrue(self.feed(b'{"\\u006fp":"ping"}')[0]["ok"])      # escaped keys still match

    def test_numbers(self):
        limit = 2147483647
        for key in ("offset", "limit", "timeout_ms", "mode"):
            for num in (b"0", b"-0", b"1", b"-1", b"%d" % limit, b"%d" % -limit, b"%d" % (limit + 1),
                        b"%d" % (-limit - 1), b"2147483648", b"4294967295", b"4294967296",
                        b"9223372036854775807", b"9223372036854775808", b"18446744073709551616", b"1" * 400,
                        b"-" + b"9" * 400, b"1.5", b"1.0", b"1e3", b"1E3", b"0x10", b"+1", b"--1", b"-", b".5",
                        b"1.", b"NaN", b"Infinity", b"0" * 400, b'"12"', b"true", b"null", b"[1]"):
                with self.subTest(key=key, num=num):
                    self.feed(b'{"op":"nonsense","%s":%s}' % (key.encode(), num))
        self.assertIn("number out of range", self.rejected(b'{"op":"ping","offset":2147483648}')["error"]["message"])
        self.assertIn("number out of range", self.rejected(b'{"op":"ping","timeout_ms":99999999999999999999}')["error"]["message"])
        self.assertTrue(self.feed(b'{"op":"ping","limit":-1,"mode":420}')[0]["ok"])

    def test_numbers_reach_file_ops_safely(self):
        path = os.path.join(self.tmp, "f.bin").encode()
        with open(path, "wb") as f:
            f.write(b"0123456789")
        for num in (b"-1", b"-2147483647", b"2147483647", b"1000000"):
            for key in (b"offset", b"max"):
                with self.subTest(key=key, num=num):
                    self.feed(b'{"op":"fs.read","path":"%s","%s":%s}' % (path, key, num))
        self.assertEqual(self.feed(b'{"op":"fs.read","path":"%s","offset":3,"limit":2}' % path)[0]["ok"], True)
        self.feed(b'{"op":"exec","cmd":"true","timeout_ms":-5}')
        self.feed(b'{"op":"fs.write","path":"%s","data":"","mode":-1}' % path)

    def test_bad_base64(self):
        path = os.path.join(self.tmp, "w.bin").encode()
        for data in (b"!!!!", b"=", b"====", b"A", b"AAAAA", b"QQ=", b"\xff\xff", b'\\u00ff', b"A" * 100001):
            with self.subTest(data=data[:20]):
                self.feed(b'{"op":"fs.write","path":"%s","data":"%s"}' % (path, data))
        self.feed(b'{"op":"fs.write","path":"%s","data":"QQ==","more":true}' % path)   # left open, then abandoned
        self.feed(b'{"op":"ping"}')

    def test_random_mutations(self):
        rng = random.Random(1234)
        seeds = [b'{"id":7,"op":"exec","cmd":"echo hi","timeout_ms":2000}',
                 b'{"id":"x","op":"fs.stat","path":"/"}',
                 b'{"id":3,"op":"fs.read","path":"/proc/version","offset":1,"limit":50}',
                 b'{"id":4,"op":"fs.list","path":"/"}',
                 b'{"id":5,"op":"spawn","cmd":"sleep 0.01","mem_mb":64,"nproc":4,"cpu_s":5}',
                 b'{"id":6,"op":"job.tail","job_id":1,"offset":0}',
                 b'{"op":"fs.write","path":"/nonexistent/x","data":"QUJD","mode":420,"append":true,"more":false}']
        alphabet = b'{}[]",:\\u0123456789-.eEtfn \x00\xff\n'.replace(b"\n", b" ")
        for i in range(400):
            data = bytearray(rng.choice(seeds))
            for _ in range(rng.randint(1, 6)):
                pos = rng.randrange(len(data) + 1)
                kind = rng.randrange(3)
                if kind == 0 and data:
                    del data[min(pos, len(data) - 1)]
                elif kind == 1:
                    data.insert(pos, rng.choice(alphabet))
                elif data:
                    data[min(pos, len(data) - 1)] = rng.choice(alphabet)
            with self.subTest(i=i, case=bytes(data)):
                self.feed(bytes(data))


if __name__ == "__main__":
    unittest.main()
