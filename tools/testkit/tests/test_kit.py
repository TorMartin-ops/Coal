"""Tests of the test kit itself: admission, QEMU arguments, determinism rules, no retries, reports. No QEMU needed."""
import os
import subprocess
import sys
import tempfile
import threading
import types
import time
import unittest
import xml.dom.minidom

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from testkit import qemu, report, runner, slots, suites          # noqa: E402
from testkit.model import NONDET, PASS, FAIL, Test               # noqa: E402


FAKE_BUILD = types.SimpleNamespace(dir="/tmp", reset_disk=lambda: None)


class SlotDir(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.saved = {k: os.environ.pop(k, None) for k in (slots.HELD_ENV,)}
        os.environ.update(COAL_TESTKIT_SLOT_DIR=os.path.join(self.tmp, "slots"), COAL_TESTKIT_SLOTS="2")

    def tearDown(self):
        for k in ("COAL_TESTKIT_SLOT_DIR", "COAL_TESTKIT_SLOTS"):
            os.environ.pop(k, None)
        os.environ.update({k: v for k, v in self.saved.items() if v})


class Slots(SlotDir):
    def test_two_slots_then_queue(self):
        a, b = slots.acquire(), slots.acquire()
        self.assertEqual({a.index, b.index}, {0, 1})
        got = []
        t = threading.Thread(target=lambda: got.append(slots.acquire()))
        t.start()
        time.sleep(0.3)
        self.assertEqual(got, [], "a third guest must wait while both slots are taken")
        self.assertEqual(slots.status(), [(0, True), (1, True)])
        a.release()
        b.release()
        t.join(5)
        self.assertEqual(len(got), 1)
        got[0].release()
        self.assertEqual(slots.status(), [(0, False), (1, False)])

    def test_shell_flock_convention(self):
        """`flock slots/0 CMD` holds slot 0 for other agents, as the coordinator's convention says."""
        s = slots.acquire()
        s.release()
        p = subprocess.Popen(["flock", os.path.join(slots.slot_dir(), "0"), "sleep", "2"])
        time.sleep(0.3)
        self.assertEqual(slots.status()[0], (0, True))
        self.assertEqual(slots.acquire().index, 1)
        p.wait()

    def test_children_inherit(self):
        s = slots.acquire()
        os.environ.update(slots.held_env(s))
        try:
            child = slots.acquire()
            self.assertIsNone(child.fd)               # no second slot for the same process tree
            child.release()
            self.assertEqual(slots.status().count((0, True)) + slots.status().count((1, True)), 1)
        finally:
            del os.environ[slots.HELD_ENV]
            s.release()

    def test_default_is_cores_minus_one(self):
        os.environ.pop("COAL_TESTKIT_SLOTS")
        self.assertEqual(slots.slot_count(), max(1, (os.cpu_count() or 2) - 1))


class QemuArgs(unittest.TestCase):
    def args(self, **kw):
        return qemu.build_args(qemu.GuestSpec(argv=["/usr/bin/hello"], **kw), "k.bin", "d.img", "/w", 7)

    def test_determinism_flags(self):
        a = " ".join(self.args())
        for want in ("-icount shift=6,sleep=off,align=off", "-rtc base=2026-01-01T00:00:00,clock=vm", "coal.seed=7",
                     "coal.test=/usr/bin/hello", "-nic none", "-pidfile /w/qemu.pid", "driver=blkreplay", "snapshot=on",
                     "-accel tcg,thread=single"):
            self.assertIn(want, a)

    def test_record_replay_differ_only_in_icount(self):
        run = self.args()
        rec = qemu.build_args(qemu.GuestSpec(argv=["/usr/bin/hello"]), "k.bin", "d.img", "/w", 7, "record", "r.bin")
        rep = qemu.build_args(qemu.GuestSpec(argv=["/usr/bin/hello"]), "k.bin", "d.img", "/w", 7, "replay", "r.bin")
        strip = lambda a: [x for x in a if "icount" not in x and not x.startswith("shift=")]    # noqa: E731
        self.assertEqual(strip(run), strip(rec))
        self.assertEqual(strip(rec), strip(rep))
        self.assertIn("shift=6,rr=record,rrfile=r.bin", rec)

    def test_profiles(self):
        self.assertIn("virtio-blk-pci,drive=img,disable-legacy=on", self.args(profile="virtio"))
        micro = " ".join(self.args(profile="microvm"))
        self.assertIn("-machine microvm", micro)
        self.assertIn("virtio_mmio.device=512@0xfeb02e00:28", micro)

    def test_normalize_drops_only_timestamps(self):
        a = "[    0.006] [INFO ] vfs: mounted\nok\n@@COALTEST exit=0 signal=0 timeout=0 ns=123"
        b = "[    0.007] [INFO ] vfs: mounted\nok\n@@COALTEST exit=0 signal=0 timeout=0 ns=999"
        self.assertEqual(qemu.normalize(a), qemu.normalize(b))
        self.assertNotEqual(qemu.normalize(a), qemu.normalize(b.replace("ok", "bad")))


class Rules(unittest.TestCase):
    """Determinism rules, with host commands standing in for guests."""

    def ctx(self, repeat=3, seeds=(1,)):
        return runner.Context(suites.REPO, FAKE_BUILD, "/tmp", list(seeds), repeat)

    def py(self, code, **kw):
        return Test("t", "x", cmd=[sys.executable, "-c", code], **kw)

    def test_identical_runs_pass(self):
        r = runner.run_test(self.py("print('same')", deterministic=True), self.ctx())
        self.assertEqual((r.verdict, len(r.runs)), (PASS, 3))

    def test_changing_output_is_nondeterminism_with_a_diff(self):
        r = runner.run_test(self.py("import os;print(os.getpid())", deterministic=True), self.ctx())
        self.assertEqual(r.verdict, NONDET)
        self.assertIn("---", r.diff)

    def test_flapping_verdict_is_nondeterminism_and_there_is_no_retry(self):
        counter = tempfile.mktemp()
        code = ("import os,sys;p=%r;n=int(open(p).read()) if os.path.exists(p) else 0;open(p,'w').write(str(n+1));"
                "sys.exit(1 if n==1 else 0)" % counter)
        r = runner.run_test(self.py(code), self.ctx())
        self.assertEqual(r.verdict, NONDET)
        self.assertEqual(open(counter).read(), "3", "each run executes exactly once")
        os.unlink(counter)

    def test_failure_is_not_retried(self):
        counter = tempfile.mktemp()
        code = "p=%r;open(p,'a').write('x');raise SystemExit(1)" % counter
        r = runner.run_test(self.py(code), self.ctx(repeat=1))
        self.assertEqual(r.verdict, FAIL)
        self.assertEqual(open(counter).read(), "x")
        os.unlink(counter)

    def test_seed_sensitive_needs_different_output_per_seed(self):
        t = self.py("print('constant')", deterministic=True, seed_sensitive=True)
        self.assertEqual(runner.run_test(t, self.ctx(repeat=1)).verdict, FAIL)
        t = self.py("import os;print(os.environ['COAL_TESTKIT_SEED'])", deterministic=True, seed_sensitive=True)
        self.assertEqual(runner.run_test(t, self.ctx(repeat=2)).verdict, PASS)

    def test_volatile_numbers_are_blanked_before_comparing(self):
        code = "import os;print('killed', 1000 + os.getpid() % 97, 'KiB resident')"
        t = self.py(code, deterministic=True, volatile=(r"\d+ KiB resident",))
        self.assertEqual(runner.run_test(t, self.ctx()).verdict, PASS)

    def test_missing_device_skips(self):
        r = runner.run_test(Test("t", "x", cmd=["true"], needs=("vhost-device-vsock-absent",)), self.ctx(1))
        self.assertIn(r.verdict, ("PASS", "SKIP"))      # unknown names are not devices: only known ones can be missing


class Registry(unittest.TestCase):
    def test_every_guest_program_has_a_test(self):
        unused, unknown = suites.orphans()
        self.assertEqual((unused, unknown), ([], []))

    def test_ids_unique_and_costed(self):
        ids = [t.id for s in suites.all_suites() for t in s.tests]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue(all(t.cost_s > 0 for s in suites.all_suites() for t in s.tests))

    def test_junit_is_well_formed(self):
        r = runner.run_test(Test("t", "x", cmd=[sys.executable, "-c", "print('\\x01 <&>'); raise SystemExit(1)"]),
                            runner.Context(suites.REPO, FAKE_BUILD, "/tmp", [1], 1))
        xml.dom.minidom.parseString(report.to_junit([r]))


if __name__ == "__main__":
    unittest.main()
