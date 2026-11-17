#!/usr/bin/env python3
"""Unit tests for the frame codec, the address parser and the run log redaction (no VM needed)."""
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from coal_gateway import agent, errors, frames, machine, runlog, transport  # noqa: E402


class Codec(unittest.TestCase):
    def test_encode_is_length_prefixed_little_endian_json(self):
        data = frames.encode({"id": 1, "op": "ping"})
        self.assertEqual(data[:4], struct.pack("<I", len(data) - 4))
        self.assertEqual(json.loads(data[4:]), {"id": 1, "op": "ping"})

    def test_non_ascii_is_escaped(self):
        self.assertTrue(frames.encode({"s": "héllo \U0001F600"})[4:].isascii())

    def test_encode_refuses_oversize(self):
        with self.assertRaises(frames.FrameError):
            frames.encode({"d": "x" * 100}, max_frame=50)

    def test_decoder_handles_any_split_and_glued_frames(self):
        stream = b"".join(frames.encode({"id": i}) for i in range(5))
        for step in (1, 3, 7, len(stream)):
            d, got = frames.Decoder(), []
            for i in range(0, len(stream), step):
                got += d.feed(stream[i:i + step])
            self.assertEqual([m["id"] for m in got], list(range(5)), step)

    def test_decoder_survives_bad_bodies_and_counts_them(self):
        d = frames.Decoder()
        raw = struct.pack("<I", 3) + b"{{{" + struct.pack("<I", 2) + b"[]" + struct.pack("<I", 0) + frames.encode({"ok": 1})
        self.assertEqual(d.feed(raw), [{"ok": 1}])
        self.assertEqual(d.dropped, 2)

    def test_decoder_rejects_an_oversize_length_before_the_body_arrives(self):
        with self.assertRaises(frames.FrameError):
            frames.Decoder(max_frame=100).feed(struct.pack("<I", 101))

    def test_partial_frame_waits(self):
        d = frames.Decoder()
        data = frames.encode({"a": 1})
        self.assertEqual(d.feed(data[:-1]), [])
        self.assertEqual(d.feed(data[-1:]), [{"a": 1}])


class Addresses(unittest.TestCase):
    def test_parse(self):
        self.assertIsInstance(transport.parse_address("unix:/tmp/x"), transport.SocketTransport)
        self.assertIsInstance(transport.parse_address("tcp:127.0.0.1:9"), transport.SocketTransport)
        self.assertIsInstance(transport.parse_address("hvsock:/tmp/v.uds:1024"), transport.HybridVsockTransport)

    def test_bad(self):
        for bad in ("", "unix:", "tcp:host", "tcp::9", "serial:/dev/x", "vsock:x:1", "hvsock:/tmp/x", "hvsock::1"):
            with self.assertRaises(ValueError, msg=bad):
                transport.parse_address(bad)


class LostLink(unittest.TestCase):
    """The error for a cut-off request says why the link ended."""

    class Scripted(transport.Transport):
        def __init__(self, chunks, error=None):
            self.chunks, self.error = list(chunks), error
            self.sent = __import__("threading").Event()

        def connect(self, timeout=5.0):
            pass

        def send(self, data):
            self.sent.set()

        def recv(self):
            self.sent.wait(2)                      # let the request register before the link ends
            if self.error:
                raise self.error
            return self.chunks.pop(0) if self.chunks else b""

        def close(self):
            pass

    def lost_message(self, tr, explain=None):
        c = agent.AgentClient(tr)
        c.explain = explain
        with self.assertRaises(errors.ToolError) as cm:
            c.request("ping", timeout=2)
        return str(cm.exception)

    def test_end_of_stream(self):
        self.assertIn("other end closed the connection", self.lost_message(self.Scripted([])))

    def test_frame_error_shows_the_first_bytes(self):
        msg = self.lost_message(self.Scripted([b"\xff\xff\xff\x7fjunk"]))
        self.assertIn("bad frame from agentd", msg)
        self.assertIn("ff ff ff 7f 6a 75 6e 6b", msg)

    def test_read_error_and_explanation(self):
        msg = self.lost_message(self.Scripted([], ConnectionResetError("reset by peer")), lambda: "QEMU killed by signal 9")
        self.assertIn("read failed", msg)
        self.assertIn("reset by peer", msg)
        self.assertTrue(msg.endswith("QEMU killed by signal 9"))

    def test_qemu_fate_reports_exit_code_and_stderr(self):
        import subprocess
        m = machine.Machine()
        m.proc = subprocess.Popen(["sh", "-c", "echo boom >&2; exit 3"], stderr=subprocess.PIPE)
        m.qemu_stderr = machine.collections.deque(maxlen=20)
        m.stderr_thread = machine.threading.Thread(target=machine._drain, args=(m.proc.stderr, m.qemu_stderr))
        m.stderr_thread.start()
        m.proc.wait()
        text = m.qemu_fate()
        self.assertIn("exited with code 3", text)
        self.assertIn("boom", text)


class VirtioOptions(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(machine.parse_virtio(None), {})
        self.assertEqual(machine.parse_virtio("blk, rng"), {"blk": 3, "rng": 3})
        self.assertEqual(machine.parse_virtio("vsock=42")["vsock"], 42)

    def test_bad(self):
        for bad in ("net", "blk=1", "vsock=x"):
            with self.assertRaises(ValueError, msg=bad):
                machine.parse_virtio(bad)

    def test_link_priority(self):
        link = lambda spec: machine.Machine(virtio=spec)._virtio_setup("d.img", "c.sock")
        args, cmdline, address = link("blk,console")
        self.assertEqual((cmdline, address), (["root=vda", "agentd.link=/dev/hvc0"], "unix:c.sock"))
        self.assertIn("virtconsole,chardev=hvc0", args)
        args, cmdline, address = link("blk,console,vsock=7")
        self.assertEqual((cmdline[-1], address), ("agentd.link=vsock:1024", "vsock:7:1024"))
        self.assertIn("vhost-vsock-pci,guest-cid=7,disable-legacy=on", args)
        self.assertEqual(link("rng")[1:], (["root=hda"], "unix:c.sock"))


class Redaction(unittest.TestCase):
    def test_payloads_become_digests(self):
        r = runlog.redact({"path": "/x", "content": "secret text", "content_base64": "AAAA"})
        self.assertEqual(r["path"], "/x")
        self.assertEqual(r["content"]["bytes"], 11)
        self.assertNotIn("secret text", json.dumps(r))

    def test_env_values_and_secret_keys_are_masked(self):
        r = runlog.redact({"env": {"A": "1", "B": "2"}, "api_key": "k", "Authorization": "x", "nested": {"password": "p"}})
        self.assertEqual(r["env"], {"A": "[redacted]", "B": "[redacted]"})
        self.assertEqual((r["api_key"], r["Authorization"], r["nested"]["password"]), ("[redacted]",) * 3)

    def test_free_text_is_scrubbed(self):
        r = runlog.redact({"command": "curl -H 'Authorization: Bearer abc.DEF-1' https://u:pw@h/x; TOKEN=zz run --password=hunter2"})
        for leak in ("abc.DEF-1", "pw@", "zz", "hunter2"):
            self.assertNotIn(leak, r["command"])

    def test_long_strings_are_cut(self):
        self.assertLess(len(runlog.redact({"command": "x" * 5000})["command"]), 1100)

    def test_run_ids(self):
        for ok in ("a", "run-1", "2026.11_x"):
            self.assertTrue(runlog.valid_run_id(ok))
        for bad in ("", "../x", "a/b", ".hidden", "x" * 65, None, 5):
            self.assertFalse(runlog.valid_run_id(bad))


class Log(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="runlog-")
        self.log = runlog.RunLog(os.path.join(self.tmp, "runs"), lambda: "ws-test")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def rows(self, run_id):
        with open(self.log.path(run_id)) as f:
            return [json.loads(line) for line in f]

    def test_record_shape_and_permissions(self):
        self.log.record("r1", "coal_run", {"command": "echo hi", "env": {"K": "v"}}, "exit code: 0", False, 1000.0, boot=2)
        (row,) = self.rows("r1")
        self.assertEqual((row["run_id"], row["seq"], row["tool"], row["is_error"], row["boot"], row["workspace_id"]),
                         ("r1", 1, "coal_run", False, 2, "ws-test"))
        self.assertEqual(row["result"]["bytes"], 12)
        self.assertEqual(len(row["result"]["sha256"]), 64)
        self.assertEqual(row["args"]["env"], {"K": "[redacted]"})
        self.assertNotIn("exit code", json.dumps(row))             # the result itself is not stored
        self.assertEqual(os.stat(self.log.path("r1")).st_mode & 0o777, 0o600)
        self.assertEqual(os.stat(self.log.directory).st_mode & 0o777, 0o700)

    def test_seq_continues_across_processes(self):
        self.log.record("r2", "a", {}, "", False, 0)
        again = runlog.RunLog(self.log.directory, lambda: "ws-test")
        again.record("r2", "b", {}, "", False, 0)
        self.assertEqual([r["seq"] for r in self.rows("r2")], [1, 2])


if __name__ == "__main__":
    unittest.main()
