#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""make analyze: the static analysers over hangar's C code (tools/analyze/README.md).

Runs, of clang's static analyzer, gcc -fanalyzer, cppcheck and clang-tidy, those
that are installed, over every .c file under lib/, hangard/, hangar/, modules/
and tools/ that is not a test, each with the flags the build compiles it with
(read from `make -n`). A finding is one line: file:line: [tool:checker] text.
Findings matched by a line of tools/analyze/suppress-<tool>.txt are dropped;
whatever is left is printed and the exit status is 1. A suppression that
matched nothing is noted, and does not fail the run.

  tools/analyze/analyze.py [--tools clang,gcc,cppcheck,tidy] [--jobs N] [--raw] [file.c ...]

--raw prints the findings before the suppressions (for triage).
"""
import argparse
import concurrent.futures
import fnmatch
import os
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DIRS = ["lib", "hangard", "hangar", "modules", "tools"]
HERE = os.path.join("tools", "analyze")

# Sources no `make all` compile line covers: built by a rule that compiles and
# links at once (msggen), or only into the suites (the manager stub). The two
# of tools/equivalence (`make equivalence`) take FALLBACK, lib/vision's flags.
EXTRA_FLAGS = {
    "tools/msggen/msggen.c": ["-std=c11"],
    "lib/hg/hg_manager_stub.c": ["-std=c11", "-Ilib/hg", "-pthread"],
}
FALLBACK = ["-std=gnu99", "-D_FILE_OFFSET_BITS=64", "-Ilib/common", "-Ilib/vision", "-Ilib/camera", "-Ilib/proc",
            "-Ilib/net", "-Ilib/video", "-Ilib/hg", "-pthread"]

# clang's static analyzer: its defaults (core, unix, security, nullability),
# plus the bounds and enum-range checkers that are off by default. Off: the
# checker that calls every memcpy and snprintf insecure, and dead stores
# (style: `char *line = strtok_r(...)` before the loop that reassigns it).
CLANG_CHECKERS = [
    "-Xclang", "-analyzer-disable-checker", "-Xclang", "security.insecureAPI.DeprecatedOrUnsafeBufferHandling",
    "-Xclang", "-analyzer-disable-checker", "-Xclang", "deadcode.DeadStores",
    "-Xclang", "-analyzer-checker", "-Xclang", "security.ArrayBound",
    "-Xclang", "-analyzer-checker", "-Xclang", "optin.core.EnumCastOutOfRange",
    "-Xclang", "-analyzer-checker", "-Xclang", "optin.portability.UnixAPI",
    "-Xclang", "-analyzer-checker", "-Xclang", "alpha.core.SizeofPtr",
]
# clang-tidy: bugprone-* and two misc checks, less the checks that are about
# style or flag idioms this code uses on purpose (README "The checker sets").
TIDY_CHECKS = ",".join([
    "-*", "bugprone-*", "misc-redundant-expression", "misc-misplaced-const",
    "-bugprone-easily-swappable-parameters", "-bugprone-narrowing-conversions", "-bugprone-reserved-identifier",
    "-bugprone-implicit-widening-of-multiplication-result", "-bugprone-assignment-in-if-condition",
    "-bugprone-assignment-in-selection-statement", "-bugprone-multi-level-implicit-pointer-conversion",
    "-bugprone-branch-clone", "-bugprone-switch-missing-default-case", "-bugprone-unhandled-code-paths",
    "-bugprone-casting-through-void", "-bugprone-macro-parentheses", "-bugprone-suspicious-include",
    "-bugprone-inc-dec-in-conditions", "-bugprone-command-processor", "-bugprone-signal-handler",
    "-bugprone-signed-bitwise", "-bugprone-unchecked-string-to-number-conversion",
    "-bugprone-suspicious-string-compare", "-bugprone-not-null-terminated-result", "-bugprone-unsafe-functions",
])


def sources():
    out = []
    for d in DIRS:
        for base, dirs, files in os.walk(d):
            dirs[:] = sorted(x for x in dirs if x != "tests")
            out += [os.path.join(base, f) for f in sorted(files) if f.endswith(".c")]
    return out


def build_flags():
    """Per source, the -I/-D/-std flags of its compile line in the build."""
    env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL")}
    p = subprocess.run(["make", "-n", "-B", "all"], capture_output=True, text=True, cwd=ROOT, env=env)
    if p.returncode != 0:
        sys.exit("analyze: `make -n -B all` failed, so the build's flags are not known:\n" + p.stderr)
    flags = {}
    for line in p.stdout.splitlines():
        if " -c " not in line:
            continue
        try:
            a = shlex.split(line)
        except ValueError:
            continue
        if not a[-1].endswith(".c"):
            continue
        flags[a[-1]] = [x for x in a[1:-1] if x.startswith(("-I", "-D", "-U", "-std", "-pthread"))]
    return flags


def norm(path):
    path = os.path.normpath(path)
    return os.path.relpath(path, ROOT) if os.path.isabs(path) else path


DIAG = re.compile(r"^([^:\s][^:]*):(\d+):(?:\d+:)? (warning|error): (.*?)(?: \[([^\]]+)\])?$")


def run_clang(src, fl):
    p = subprocess.run(["clang", "--analyze", "--analyzer-output", "text", "-Xclang", "-analyzer-output=text"]
                       + CLANG_CHECKERS + fl + ["-o", "/dev/null", src], capture_output=True, text=True, cwd=ROOT)
    out = []
    for line in p.stderr.splitlines():
        m = DIAG.match(line)
        if m and m.group(5):
            out.append((norm(m.group(1)), int(m.group(2)), "clang:" + m.group(5), m.group(4)))
        elif m and m.group(3) == "error":
            out.append((norm(m.group(1)), int(m.group(2)), "clang:does-not-compile", m.group(4)))
    return out


def run_gcc(src, fl):
    p = subprocess.run(["gcc", "-fanalyzer", "-fdiagnostics-plain-output", "-O1", "-c", "-o", "/dev/null"] + fl + [src],
                       capture_output=True, text=True, cwd=ROOT)
    out = []
    for line in p.stderr.splitlines():
        m = DIAG.match(line)
        if m and m.group(5) and m.group(5).startswith("-Wanalyzer"):
            out.append((norm(m.group(1)), int(m.group(2)), "gcc:" + m.group(5)[2:].split("=")[0], m.group(4)))
        elif m and m.group(3) == "error":
            out.append((norm(m.group(1)), int(m.group(2)), "gcc:does-not-compile", m.group(4)))
    return out


def run_cppcheck(src, fl):
    inc = [x for x in fl if x.startswith(("-I", "-D", "-U"))]
    std = "--std=c11" if "-std=c11" in fl else "--std=c99"
    p = subprocess.run(["cppcheck", "--enable=warning,portability", "--inline-suppr", std, "--quiet",
                        "--suppress=missingIncludeSystem", "--suppress=missingInclude", "-D__linux__",
                        "--template={file}:{line}: warning: {message} [{id}]"] + inc + [src],
                       capture_output=True, text=True, cwd=ROOT)
    out = []
    for line in p.stderr.splitlines():
        m = DIAG.match(line)
        if m and m.group(5):
            out.append((norm(m.group(1)), int(m.group(2)), "cppcheck:" + m.group(5), m.group(4)))
    return out


def run_tidy(src, fl):
    p = subprocess.run(["clang-tidy", "--quiet", "--checks=" + TIDY_CHECKS, src, "--"] + fl,
                       capture_output=True, text=True, cwd=ROOT)
    out = []
    for line in p.stdout.splitlines():
        m = DIAG.match(line)
        if m and m.group(5) and not m.group(5).startswith("clang-diagnostic"):
            out.append((norm(m.group(1)), int(m.group(2)), "tidy:" + m.group(5).split(",")[0], m.group(4)))
    return out


TOOLS = {"clang": ("clang", run_clang), "gcc": ("gcc", run_gcc), "cppcheck": ("cppcheck", run_cppcheck),
         "tidy": ("clang-tidy", run_tidy)}


def suppressions(tool):
    """Lines of `<checker> <file glob> [text the message contains] # reason`."""
    path = os.path.join(ROOT, HERE, "suppress-%s.txt" % tool)
    out = []
    if not os.path.exists(path):
        return out
    for n, line in enumerate(open(path), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "#" not in line or not line.split("#", 1)[1].strip():
            sys.exit("%s:%d: a suppression needs its reason after '#'" % (path, n))
        spec = line.split("#", 1)[0].split(None, 2)
        if len(spec) < 2:
            sys.exit("%s:%d: expected: <checker> <file glob> [text] # reason" % (path, n))
        out.append([spec[0], spec[1], spec[2].strip() if len(spec) > 2 else "", 0, "%s:%d" % (path, n)])
    return out


def suppressed(f, sup):
    path, _line, checker, msg = f
    for s in sup:
        if checker.split(":", 1)[1] == s[0] and fnmatch.fnmatch(path, s[1]) and s[2] in msg:
            s[3] += 1
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tools", default="clang,gcc,cppcheck,tidy")
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("ANALYZE_JOBS", "0")) or os.cpu_count() or 1)
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("files", nargs="*")
    args = ap.parse_args()
    os.chdir(ROOT)
    flags = build_flags()
    srcs = [norm(f) for f in args.files] or sources()
    left = 0
    for tool in args.tools.split(","):
        exe, fn = TOOLS[tool]
        if not shutil.which(exe):
            print("skip  analyze %s: %s is not installed" % (tool, exe))
            continue
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
            res = list(ex.map(lambda s: fn(s, flags.get(s) or EXTRA_FLAGS.get(s) or FALLBACK), srcs))
        found = sorted(set(f for r in res for f in r if not f[0].startswith(("/", ".."))))
        sup = suppressions(tool)
        shown = found if args.raw else [f for f in found if not suppressed(f, sup)]
        for f in shown:
            print("%s:%d: [%s] %s" % f)
        stale = [s for s in sup if not s[3]] if not args.raw and not args.files else []
        for s in stale:
            print("note: %s matches nothing (this version of %s does not report it, or the code changed): "
                  "remove the line if it is the code" % (s[4], exe))
        left += len(shown)
        print("%s  analyze %s  %d files, %d findings, %d suppressed" %
              ("FAIL" if shown else "ok   ", tool, len(srcs), len(found), len(found) - len(shown) if not args.raw else 0))
    return 1 if left and not args.raw else 0


if __name__ == "__main__":
    sys.exit(main())
