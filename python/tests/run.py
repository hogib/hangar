#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The Python suite as `make check` runs it: every test of python/tests
(unittest discovery), with the skips said out loud.

    python3 python/tests/run.py [--log FILE]

Most of hangar.learn's and hangar.nn's tests need numpy, and the export and
example tests PyTorch; without them they skip, and `unittest discover` then
says OK. So this prints one line with the number of tests run and, when any
skipped, how many and why, by reason:

    ok    python  126 tests
    ok    python  82 tests, 44 SKIPPED: numpy is not installed (37), PyTorch is not installed (7)

and with HANGAR_PYTHON_NO_SKIP=1 in the environment (CI's job that installs
numpy and PyTorch) a skipped test fails the run, each one named. A run that
finds no tests fails too. unittest's own output goes to FILE (default:
standard error) and is printed after a FAIL line.

Standard library only, as the tests it must be able to report as skipped.
"""

import argparse
import io
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def report(result, strict):
    """(exit status, the lines to print) for a unittest result."""
    reasons = {}
    for _test, why in result.skipped:
        reasons[why] = reasons.get(why, 0) + 1
    skips = ", ".join("%s (%d)" % (why, n) for why, n in sorted(reasons.items(), key=lambda kv: (-kv[1], kv[0])))
    ran = result.testsRun - len(result.skipped)
    if not result.wasSuccessful():
        return 1, ["FAIL  python  %d failed, %d errors, of %d tests" % (
            len(result.failures) + len(result.unexpectedSuccesses), len(result.errors), result.testsRun)]
    if result.testsRun == 0:
        return 1, ["FAIL  python  no tests found"]
    if strict and result.skipped:
        return 1, ["FAIL  python  %d of %d tests skipped with HANGAR_PYTHON_NO_SKIP set: %s" % (
            len(result.skipped), result.testsRun, skips)] + \
            ["      skipped: %s: %s" % (test.id(), why) for test, why in result.skipped]
    if result.skipped:
        return 0, ["ok    python  %d tests, %d SKIPPED: %s" % (ran, len(result.skipped), skips)]
    return 0, ["ok    python  %d tests" % ran]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--log", help="unittest's output goes here instead of standard error")
    a = ap.parse_args(argv)
    sys.path.insert(0, os.path.dirname(HERE))
    suite = unittest.defaultTestLoader.discover(HERE)
    buf = io.StringIO()
    result = unittest.TextTestRunner(stream=buf, verbosity=2).run(suite)
    text = buf.getvalue()
    if a.log:
        with open(a.log, "w") as f:
            f.write(text)
    else:
        sys.stderr.write(text)
    status, lines = report(result, os.environ.get("HANGAR_PYTHON_NO_SKIP", "") not in ("", "0"))
    print("\n".join(lines))
    if status and a.log:
        print(text)
    return status


if __name__ == "__main__":
    sys.exit(main())
