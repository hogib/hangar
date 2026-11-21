# SPDX-License-Identifier: LGPL-3.0-or-later
"""python/tests/run.py, the runner `make check` uses: a skipped test is
counted and its reason printed, and fails the run where skips are not
allowed (HANGAR_PYTHON_NO_SKIP, CI's job with numpy and PyTorch). The
suite printed `ok` whatever skipped: without numpy 37 of 98 tests did."""

import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run  # noqa: E402


class _Sample(unittest.TestCase):
    def passes(self):
        pass

    @unittest.skip("numpy is not installed")
    def needs_numpy(self):
        pass

    @unittest.skip("numpy is not installed")
    def needs_numpy_too(self):
        pass

    @unittest.skip("PyTorch is not installed")
    def needs_torch(self):
        pass

    def fails(self):
        self.fail("no")


def result_of(*names):
    suite = unittest.TestSuite(_Sample(n) for n in names)
    return unittest.TextTestRunner(stream=io.StringIO()).run(suite)


class Report(unittest.TestCase):
    def test_all_run(self):
        self.assertEqual(run.report(result_of("passes", "passes"), False), (0, ["ok    python  2 tests"]))
        self.assertEqual(run.report(result_of("passes"), True), (0, ["ok    python  1 tests"]))

    def test_skips_are_counted_by_reason(self):
        status, lines = run.report(result_of("passes", "needs_numpy", "needs_torch", "needs_numpy_too"), False)
        self.assertEqual(status, 0)
        self.assertEqual(lines, ["ok    python  1 tests, 3 SKIPPED: numpy is not installed (2), "
                                 "PyTorch is not installed (1)"])

    def test_a_skip_fails_where_none_is_allowed(self):
        status, lines = run.report(result_of("passes", "needs_numpy", "needs_torch"), True)
        self.assertEqual(status, 1)
        self.assertTrue(lines[0].startswith("FAIL  python  2 of 3 tests skipped"), lines)
        self.assertIn("      skipped: test_run._Sample.needs_torch: PyTorch is not installed", lines)

    def test_a_failure_and_an_empty_run_fail(self):
        status, lines = run.report(result_of("passes", "fails"), False)
        self.assertEqual((status, lines[0]), (1, "FAIL  python  1 failed, 0 errors, of 2 tests"))
        self.assertEqual(run.report(result_of(), False), (1, ["FAIL  python  no tests found"]))


if __name__ == "__main__":
    unittest.main()
