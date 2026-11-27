# tools/analyze

`make analyze` runs the static analysers that are installed over hangar's C
code and fails on any finding that is not named in a suppression file.

    make analyze                          all four, one process per core
    make analyze ANALYZE_JOBS=2           at most two processes
    make analyze ANALYZE_TOOLS=cppcheck   one tool (clang, gcc, cppcheck, tidy; comma-separated)
    tools/analyze/analyze.py --raw --tools clang hangard/control.c
                                          one file, before the suppressions: for triage

What is analysed: every `.c` under `lib/`, `hangard/`, `hangar/`, `modules/`,
`tools/` and `bench/pairs/` that is not in a `tests/` directory (101 files when
this was written). Each is analysed with the `-I`, `-D` and `-std` flags its
compile line has in `make -n -B all`, so a new module is covered when it is
built by `all`; the nodes of `bench/pairs`, which `make pairs` builds, have
their flags in `analyze.py` (`EXTRA_FLAGS`). Not analysed: the suites,
`legacy/`, the rest of `bench/`, `fuzz/`, the generated `msg/*.c`, and the two
nodes of `bench/pairs` that include iceoryx2's header, which this tree does not
carry (`SKIP`: `tools/baselines.sh` builds them, and they were run through gcc's
and clang's analysers by hand against the built header).

A tool that is not installed is skipped with a line saying so; the others
still decide the result. It is not part of `make check`: with one process it
takes four and a half minutes on a laptop, and a tool's next release may find
something new in code nobody touched.

## The checker sets

Settled on 2026-11-21 by running each tool wide, reading every finding, and
dropping what produced nothing but noise on this code.

**clang's static analyzer** (`clang --analyze`, clang 23.1.5): the default
checkers (core, unix, security, nullability), plus `security.ArrayBound`,
`optin.core.EnumCastOutOfRange`, `optin.portability.UnixAPI` and
`alpha.core.SizeofPtr`. Off: `deadcode.DeadStores` (style: an initialiser a
loop overwrites) and `security.insecureAPI.DeprecatedOrUnsafeBufferHandling`
(every `memcpy` and `snprintf`). Tried and dropped: `alpha.unix.cstring.
OutOfBounds` and `alpha.unix.cstring.BufferOverlap` (four findings, all
false: a `memcpy` whose bound is two lines up; `snprintf` into one member of
a struct from another).

**gcc -fanalyzer** (gcc 16.2.1): its defaults, at `-O1`.

**cppcheck** (2.22.0): `--enable=warning,portability`. `style` and
`performance` are off: they are about taste here.

**clang-tidy** (LLVM 23.1.5): `bugprone-*`, `misc-redundant-expression`,
`misc-misplaced-const`, less the checks listed in `analyze.py`
(`TIDY_CHECKS`). The ones removed are style (`signed-bitwise`: 376 findings on
`1 << n` and flag masks; `suspicious-string-compare`: `!strcmp(...)`;
`assignment-in-if-condition`; `switch-missing-default-case`; ...) or flag an
idiom used on purpose (`unchecked-string-to-number-conversion`: `sscanf` and
`atoi` whose result is range-checked after; `not-null-terminated-result`: a
`memcpy` of a prefix into a buffer that is terminated on the next line). The
`cert-*` group is aliases of the same checks and was left out. clang-tidy's
`clang-analyzer-*` group is the static analyzer again and is not run twice.

A wider run is one edit away (`CLANG_CHECKERS`, `TIDY_CHECKS`), and worth
repeating when a tool gains a checker.

## Suppressions

One file per tool, `suppress-<tool>.txt`, one finding or one group of the same
finding per line:

    <checker> <file glob> [text the message contains]   # the reason

A suppression has no line number, so it survives edits; it names one checker
in one file (or a glob of files), and may be narrowed by a piece of the
message. The reason is not optional: a line without one stops the run. A
suppression that matches nothing any more is reported, so that the files do
not collect lines for code that has changed (it does not fail the run: another
version of a tool may simply not report that finding).

The rule for adding one: the finding was read, the code is right, and no small
change would make it right in the tool's eyes *and* clearer to a person. No
cast, annotation or restructuring goes into the code to quiet a tool.

## What the tools did not see

They agree with each other less than one would hope: of the four findings
that were bugs, each was reported by exactly one tool. And one of the four
was first read as harmless: cppcheck's `wrongPrintfScanfArgNum` in
`lib/nn/nn_ops.c` ("format string requires 2 parameters but 3 are given") was
suppressed with "C ignores excess arguments", which is true of the call it
names and not of its neighbour, where a `%s` was handed a number. The fuzzer
crashed there the same night. A finding about one call site of a helper is a
reason to read every call site. None of them reported any of
the bugs the fuzzers found (integer overflow in a duration, a NaN converted to
an integer, a request read one byte past its end in a static buffer). They
model one translation unit at a time: what a function in another file
guarantees (`hg_next` filled this struct, `recvfrom` filled that address) is
where most of the false positives come from.

Not installed on the machine this was set up on, and what each would add:

- **scan-build's HTML reports**: nothing the text output lacks, except the
  path through the code drawn out; `clang --analyze --analyzer-output html
  -o <dir> <file>` gives one for a single file.
- **clang's cross-translation-unit analysis** (`-analyzer-config
  experimental-enable-naive-ctu-analysis=true`, with `clang-extdef-mapping`):
  would remove most of the "the analyzer does not know this call fills its
  output" suppressions; needs a compilation database and an index step.
- **Frama-C / Eva, or CBMC**: proofs of absence of run-time errors for the
  small pure parsers (`hg_type.c`, `mav.c`, `depack.c`). A different order of
  effort; the fuzz harnesses of `fuzz/` cover the same functions dynamically.
- **Coverity or CodeQL**: cross-file taint tracking from `recv` to an index or
  a length, which is the question most worth asking of this code. CodeQL is
  free for a public repository and runs as a GitHub workflow.
- **include-what-you-use, `-Wconversion`**: hygiene, not bugs.
