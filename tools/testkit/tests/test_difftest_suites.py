"""The baseline rule of the difftest suites: a new divergence or a stale entry fails, an identical set passes."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from testkit import difftest_suites as ds  # noqa: E402


class Baseline(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.saved = ds.BASELINES
        ds.BASELINES = self.dir
        self.saved_ltp, ds.LTP_DIR = ds.LTP_DIR, os.path.join(self.dir, "no-ltp")      # no tree: the row count is not checked
        with open(os.path.join(self.dir, "b.txt"), "w") as f:
            f.write("# header\na\nb\n")

    def tearDown(self):
        ds.BASELINES = self.saved
        ds.LTP_DIR = self.saved_ltp

    def test_same_set_passes(self):
        self.assertIsNone(ds._compare("b.txt", {"a", "b"}, "h"))

    def test_new_fails(self):
        self.assertIn("new", ds._compare("b.txt", {"a", "b", "c"}, "h"))

    def test_stale_fails(self):
        self.assertIn("stale", ds._compare("b.txt", {"a"}, "h"))

    def test_ltp_rows_use_host_passing_tests_only(self):
        with open(os.path.join(self.dir, "ltp_known_failures.txt"), "w") as f:
            f.write("x/a\n")
        text = "x/a  coal=brok  host=pass\nx/b  coal=brok  host=conf\nx/c  coal=pass  host=pass\n"
        self.assertIsNone(ds.check_ltp(text))
        self.assertIn("new", ds.check_ltp(text + "x/d  coal=fail  host=pass\n"))
        self.assertIn("stale", ds.check_ltp("x/a  coal=pass  host=pass\n"))
        self.assertIsNone(ds.check_ltp("x/a  coal=flaky  host=pass\n"))       # garbage-pid flake: no verdict


if __name__ == "__main__":
    unittest.main()
