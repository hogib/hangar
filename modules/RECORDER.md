# The brief: recorder, datasets, pairer, offline reader

A copy of the brief this work was done to, kept beside the modules as docs/06's
companion. The result is `modules/recorder/`, `modules/pairer/`, `lib/hg/hg_record`,
`hg_dataset` and `hg_ini`, `hangar/hangar_dataset.c`, `python/hangar/record.py` and
`dataset.py`; the deviations are in the two READMEs.

---

You are implementing the recorder, the `.hgr` recording format, datasets, the live
pairer and the offline dataset reader of hangar, per `docs/06-recorder.md`, in this
git worktree (a branch off `main` at `7a3e5ca`). Another agent is concurrently building
the phase-1 modules (safety, mux, mavlink) in the main working tree; you never touch
that tree, and you keep your changes merge-friendly: new directories, new files, and an
append-only three-line include in the root `Makefile` (see how
`modules/console/console.mk` is included and follow the same pattern with
`modules/recorder/recorder.mk`). Do not edit `lib/hg/*.c` or `lib/hg/*.h`, `hangard/`,
`msg/*` that already exist, or `tools/sim/`; if the library lacks something you need,
add a new file (`lib/hg/hg_record.[ch]` or under `modules/common/`), note it in the
README, and list the gap. New `.msg` types are fine (new files only; regenerate with
`make msg`).

Read first, in this order: `docs/06-recorder.md` in full (the spec; everything in it is
agreed), `docs/01-topics.md` (the topic file format, seqlock, provenance refs,
`first_seq`, `origin_boot`, the replaced bit, the stale flag), `docs/02-manifest.md` and
`docs/03-manager.md` as far as a module needs them (readiness, `$HANGAR_ROOT`, state
directory, `hangar.<role>.modules`), `docs/09-learning.md` for what the trainer expects
of rows and sessions, `lib/hg/hg_topic.h`, `lib/hg/hg_type.h`, `lib/hg/hg_wiring.h`,
`lib/hg/hg_sys.h`, `lib/hg/hg_params.h`, `lib/hg/NOTES.md` and `hangard/README.md`
(deviations already taken: the code is authoritative where it departs from a doc),
`lib/hg/hg_type_abi.md`, `tools/msggen/` and one generated pair under `msg/` (how types
are described and hashed), `modules/console/` as the model of a finished module
(layout, `.mk`, `.module` manifest, README with deviations, a suite using
`hangard/tests/hd_test.h` and the toy modules of `hangard/tests/`),
`python/hangar/schema.py` and `python/tests/` (the Python side's conventions), and
`lib/hg/PLAN.md`.

Deliverables:
1. `modules/recorder/` (identifier `recorder`): docs/06 "The recorder" in full as far as
   the library allows: what it records (every topic, or the manifest's list; frames per
   the "small frame in full, large frames as headers" rule), draining from the rings
   with the loss accounting the doc specifies, sessions (start/stop, the session
   directory layout, the index), the black box (the always-on ring on disk with its
   bounded size), the `.hgr` file format byte-exact as the doc describes it (if the doc
   leaves a field unspecified, choose, document in the README, and make the Python
   reader agree), `recorder.conf` parsing (strict, like the manifests), its calls
   (`start`, `stop`, `mark`, whatever the doc names, with the Allow lists), its
   parameters, its state topic. Read-only on the topics: a recorder never writes a topic
   except its own state and `.rep`.
2. Datasets: the `.dataset` spec parser (joins, filters, the row layout per "The row")
   as a library piece usable by both the pairer and the offline reader:
   `modules/common/dataset.[ch]` or `lib/hg/hg_dataset.[ch]` (LGPL if under lib/hg),
   with the row's binary layout fixed and documented so the Python trainer reads the
   same bytes.
3. `modules/pairer/` (identifier `pairer`): docs/06 "Live: the pairer": produces rows
   live from the topics by following provenance, streams them to the configured sink
   (the doc's transport; where it depends on the bridge or the topside, implement the
   local sink, a file or a UNIX/UDP socket on the same host, and mark it
   `/* until the bridge: */`), counts `missing`, `late`, and whatever else the doc
   names, exposes them in its state topic.
4. Offline: `hangar dataset <spec> <recording>` as an added subcommand if
   `hangar/hangar.c` can take it with an append-only edit (one `else if` branch and a
   function in a new file `hangar/hangar_dataset.c`); otherwise a separate small program
   `builddir/hangar-dataset`, documented. Produces the identical rows from an `.hgr`
   recording that the pairer produces live, except the `late` flag, as docs/06 and
   docs/00 promise.
5. Python: `python/hangar/record.py` (reads `.hgr`), `python/hangar/dataset.py` (reads
   the row stream and `.dataset` specs, yields numpy arrays), tests under
   `python/tests/` on files produced by the C suite and checked in as small fixtures
   (keep them under 100 KB each).
6. Message types: whatever docs/06 names for state and calls (`recorder_state`,
   `pairer_state`, the `.req`/`.rep` pairs), new files only.
7. Tests, suites in `check`, each under 30 s, as an ordinary user under a scratch
   `$HANGAR_ROOT`: the `.hgr` writer/reader round trip incl. torn-read and loss cases;
   draining under a fast toy publisher (use `hangard/tests/` toy modules or add your own
   under `modules/recorder/tests/`); sessions and the black box's bound; the dataset
   parser (good and every rejected spec form in the doc); the join on a synthetic
   provenance chain (publisher A → B → C with `hg_publish_with`) including `missing`
   and `stale`; live pairer vs offline reader byte-identical rows except `late` on the
   same run (record and pair simultaneously, then compare); the Python readers against
   the fixtures. Also `make check-tsan`/`check-asan` must pass with your new lib pieces
   if any are under lib/hg (extend the sanitizer target lists in the same append-only
   way).
8. `modules/recorder/README.md` and `modules/pairer/README.md` with deviations and the
   "until the bridge" list; a copy of this brief at `modules/RECORDER.md`. Do not touch
   `docs/names.md` (the user names programs; use the identifiers as binary names for
   now: `builddir/recorder`, `builddir/pairer`).

Rules: C11, `-std=c11 -Wall -Wextra -pedantic`, clean under gcc, clang and `zig cc`;
libc, pthreads, the kernel, lib/hg; SPDX `GPL-3.0-or-later` for modules,
`LGPL-3.0-or-later` for anything under lib/hg or python/; no allocation on hot paths
(the drain loop, the pairer's join), bounded buffers sized from config at start; the
docs' voice in READMEs (plain, no marketing). Shell: some coreutils are aliased on this
machine, so prefix them with `command` and use absolute paths, `make -C`, `git -C`.
Verify from clean scratch directories under your scratchpad: `make -j12 check` with
gcc, with clang (`CC=clang`), with zig cc (see the root Makefile and CI for how it is
invoked), all 41 existing suites plus yours, zero warnings; `make check-tsan
check-asan`; `make equivalence`; python tests as CI runs them (see
`.github/workflows/ci.yml`; run python without `-I`); static aarch64 links of
`recorder` and `pairer` via the same cross recipe `tools/release.sh` uses. Where
docs/06 is impossible, contradictory with the library as built, or depends on the
bridge or the topside trainer, implement the closest correct thing and list it under
deviations; never change behaviour silently. Commit on your branch in the worktree as
pieces land (recorder format + writer; recorder module; datasets + pairer; offline +
Python), with messages in the repo's style (see `git log`), ending each with
`Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Do not push. If you run out
of room, stop at a verified boundary, commit, and report exactly what is done and what
is not. Your final report: the branch name, the commits, which suites were added, the
verification results per compiler, the deviations list, and anything the phase-1 merge
will need to know.
