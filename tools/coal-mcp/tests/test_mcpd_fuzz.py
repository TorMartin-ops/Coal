#!/usr/bin/env python3
"""Throws malformed and hostile requests at the real mcpd (32-bit host build on a pty).
Run: python3 tools/coal-mcp/tests/test_mcpd_fuzz.py

After every request mcpd must still answer a ping, and every line it sends must be valid
UTF-8 JSON. A hang (no answer for 10 s) or a crash fails the test.
"""
import json
import os
import pty
import random
import select
import shutil
import subprocess
import tempfile
import time
import tty
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MCPD_BUILD = os.path.join(HERE, "..", "..", "..", "src", "CoalOS", "userspace", "mcpd", "test", "build_host.sh")
PING = b'{"id":"sync","op":"ping"}\n'
PONG = b'{"id":"sync","ok":true,"pong":true}'


@unittest.skipUnless(shutil.which("gcc"), "gcc not available")
class McpdFuzz(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="coal-fuzz-")
        binary = os.path.join(cls.tmp, "mcpd")
        if subprocess.call(["sh", MCPD_BUILD, binary], stderr=subprocess.DEVNULL) != 0:
            raise unittest.SkipTest("cannot build 32-bit mcpd on this host")
        cls.master, slave = pty.openpty()
        tty.setraw(cls.master)
        cls.proc = subprocess.Popen([binary, os.ttyname(slave)])
        cls.buf = b""
        cls.readline(cls)                                     # the ready event

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        os.close(cls.master)
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def readline(self, timeout=10):
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            left = deadline - time.time()
            if left <= 0 or not select.select([self.master], [], [], left)[0]:
                raise AssertionError("mcpd did not answer within %d s" % timeout)
            self.buf += os.read(self.master, 65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return line

    def send(self, data):
        view = memoryview(data)
        while view:                                           # read replies while writing: the pty buffer is small
            r, w, _ = select.select([self.master], [self.master], [], 10)
            if not r and not w:
                raise AssertionError("mcpd stopped reading")
            if r:
                self.buf += os.read(self.master, 65536)
            if w:
                view = view[os.write(self.master, view[:4096]):]

    def feed(self, data):
        """Sends data plus a newline, then a ping; returns the replies that came before the ping's."""
        self.assertIsNone(self.proc.poll(), "mcpd crashed earlier")
        self.send(data + b"\n" + PING)
        lines = []
        while True:                                           # drain up to the ping's answer before judging anything
            line = self.readline()
            if line == PONG:
                break
            lines.append(line)
        replies = [json.loads(line.decode("utf-8")) for line in lines]   # strict: invalid UTF-8 or JSON fails here
        self.assertIsNone(self.proc.poll(), "mcpd crashed")
        return replies

    def rejected(self, data):
        replies = self.feed(data)
        self.assertEqual(len(replies), 1, replies)
        self.assertFalse(replies[0]["ok"], replies[0])
        return replies[0]

    def test_malformed(self):
        for case in [b"{", b"}", b"[]", b"null", b'"', b"{{{{", b'{"op"', b'{"op":', b'{"op":"ping"',
                     b'{"op":"ping",}', b'{"op" "ping"}', b"{'op':'ping'}", b'{"op":"ping"} trailing',
                     b'{"op":tru}', b'{,}', b'{"":""}', b'{"op":"ping","op":"nope"}', b"\x00", b"{\x00}",
                     b'{"op":"pi\x00ng"}', b" ", b"\r", b'{"op":"run"}', b'{"op":"nonsense"}', b'{"op":5}',
                     b'{"op":null}', b'{"op":["ping"]}', b'{"op":{"op":"ping"}}']:
            with self.subTest(case=case):
                self.feed(case)

    def test_unknown_and_missing_op_are_errors(self):
        self.assertIn("unknown op", self.rejected(b'{"id":1,"op":"nonsense"}')["error"])
        self.assertIn("missing op", self.rejected(b'{"id":1}')["error"])

    def test_ids_are_echoed_as_valid_json(self):
        for raw in [b'"a b"', b"12", b"-3.5", b"true", b"null", b'"q\\"uote"', b'"\\u00e9"', b"ab\"cd", b"1\t",
                    b"1e999", b"[1,2]", b'{"x":1}', b'"' + b"x" * 100 + b'"', b"\x01\x02", b"\xff\xfe"]:
            with self.subTest(id=raw):
                for r in self.feed(b'{"id":' + raw + b',"op":"nonsense"}'):
                    self.assertFalse(r["ok"])

    def test_deep_nesting(self):
        for depth in (7, 8, 9, 100, 5000, 100000):
            for pair in (b"[]", b"{}"):
                nested = b"".join(b"[" if pair == b"[]" else b'{"a":' for _ in range(depth))
                nested += b"0" + b"".join(b"]" if pair == b"[]" else b"}" for _ in range(depth))
                with self.subTest(depth=depth, kind=pair):
                    self.feed(b'{"id":1,"op":"ping","x":' + nested + b"}")
                    self.feed(b'{"id":1,"op":"ping","x":' + nested[:len(nested) // 2] + b"}")

    def test_oversized_line(self):
        r = self.rejected(b'{"op":"ping","pad":"' + b"a" * (300 * 1024) + b'"}')
        self.assertIn("too large", r["error"])
        self.feed(b'{"op":"ping"}')                           # and the next request still works
        for size in (192 * 1024 - 3, 192 * 1024 - 2, 192 * 1024 - 1, 192 * 1024, 192 * 1024 + 1):
            with self.subTest(size=size):
                self.feed(b"x" * size)

    def test_many_small_requests(self):
        replies = self.feed(b"\n".join(b'{"id":%d,"op":"ping"}' % i for i in range(500)))
        self.assertEqual(len(replies), 500)

    def test_invalid_utf8(self):
        for case in [b'{"op":"stat","path":"/\xff\xfe"}', b'{"op":"list_dir","path":"\xc0\xaf"}',
                     b'{"op":"nonsense\xed\xa0\x80"}', b'{"op":"run","cmd":"echo \xf8\x88\x80\x80\x80"}',
                     b'{"op":"stat","path":"\x80\x80\x80"}', b'{"id":"\xff","op":"ping"}']:
            with self.subTest(case=case):
                self.feed(case)

    def test_unicode_escapes(self):
        for case in [b'{"op":"stat","path":"\\u"}', b'{"op":"stat","path":"\\u12"}', b'{"op":"stat","path":"\\uZZZZ"}',
                     b'{"op":"stat","path":"\\ud800"}', b'{"op":"stat","path":"\\udc00"}',
                     b'{"op":"stat","path":"\\ud800\\u0041"}', b'{"op":"stat","path":"\\ud83d\\ude00"}',
                     b'{"op":"stat","path":"\\u0000"}', b'{"op":"stat","path":"\\u0000x"}',
                     b'{"op":"stat","path":"\\q"}', b'{"op":"stat","path":"\\', b'{"op":"stat","path":"\\"}',
                     b'{"op":"stat","path":"' + b"\\u00e9" * 20000 + b'"}', b'{"\\u006fp":"ping"}']:
            with self.subTest(case=case[:60]):
                self.feed(case)
        self.assertTrue(self.feed(b'{"\\u006fp":"ping"}')[0]["ok"])      # escaped keys still match

    def test_numbers(self):
        limit = 2147483647
        for key in ("offset", "max", "timeout_ms", "mode"):
            for num in (b"0", b"-0", b"1", b"-1", b"%d" % limit, b"%d" % -limit, b"%d" % (limit + 1),
                        b"%d" % (-limit - 1), b"2147483648", b"4294967295", b"4294967296",
                        b"9223372036854775807", b"9223372036854775808", b"18446744073709551616", b"1" * 400,
                        b"-" + b"9" * 400, b"1.5", b"1.0", b"1e3", b"1E3", b"0x10", b"+1", b"--1", b"-", b".5",
                        b"1.", b"NaN", b"Infinity", b"0" * 400, b'"12"', b"true", b"null", b"[1]"):
                with self.subTest(key=key, num=num):
                    self.feed(b'{"op":"nonsense","%s":%s}' % (key.encode(), num))
        self.assertIn("number out of range", self.rejected(b'{"op":"ping","offset":2147483648}')["error"])
        self.assertIn("number out of range", self.rejected(b'{"op":"ping","timeout_ms":99999999999999999999}')["error"])
        self.assertTrue(self.feed(b'{"op":"ping","max":-1,"mode":420}')[0]["ok"])

    def test_numbers_reach_file_ops_safely(self):
        path = os.path.join(self.tmp, "f.bin").encode()
        with open(path, "wb") as f:
            f.write(b"0123456789")
        for num in (b"-1", b"-2147483647", b"2147483647", b"1000000"):
            for key in (b"offset", b"max"):
                with self.subTest(key=key, num=num):
                    self.feed(b'{"op":"read_file","path":"%s","%s":%s}' % (path, key, num))
        self.assertEqual(self.feed(b'{"op":"read_file","path":"%s","offset":3,"max":2}' % path)[0]["ok"], True)
        self.feed(b'{"op":"run","cmd":"true","timeout_ms":-5}')
        self.feed(b'{"op":"write_file","path":"%s","data":"","mode":-1}' % path)

    def test_bad_base64(self):
        path = os.path.join(self.tmp, "w.bin").encode()
        for data in (b"!!!!", b"=", b"====", b"A", b"AAAAA", b"QQ=", b"\xff\xff", b'\\u00ff', b"A" * 100001):
            with self.subTest(data=data[:20]):
                self.feed(b'{"op":"write_file","path":"%s","data":"%s"}' % (path, data))
        self.feed(b'{"op":"write_file","path":"%s","data":"QQ==","more":true}' % path)   # left open, then abandoned
        self.feed(b'{"op":"ping"}')

    def test_random_mutations(self):
        rng = random.Random(1234)
        seeds = [b'{"id":7,"op":"run","cmd":"echo hi","timeout_ms":2000}',
                 b'{"id":"x","op":"stat","path":"/"}',
                 b'{"id":3,"op":"read_file","path":"/proc/version","offset":1,"max":50}',
                 b'{"id":4,"op":"list_dir","path":"/"}',
                 b'{"op":"write_file","path":"/nonexistent/x","data":"QUJD","mode":420,"append":true,"more":false}']
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
