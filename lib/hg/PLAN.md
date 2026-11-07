# lib/hg: the hangar module library, in stages

The library every module links (LGPL-3.0-or-later). Contract: `docs/01-topics.md`,
with the wiring and open protocol of `docs/03-manager.md` and the call topics of
`docs/04-calls.md`. C11, `<stdatomic.h>`, no dependencies beyond musl/glibc and the
kernel; built by the root Makefile with gcc, clang and `zig cc`, natively and as static
musl for aarch64 and armv7. The C spike (`spike/c/src/hg_topic.[ch]`, 920 lines, 0 torn
reads over 250 M, 0 lost wake-ups) is the starting point: its seqlock, publish order,
`hg_next` gap rule, `futex_waitv` wait, interrupt word and header validation are kept;
its deviations from the design are resolved in the design's favour where the design
has since adopted them (publish step 4 order, "flags changed" readiness, atomic word
copy, late joiners at the newest message).

## Stage 1: topics without a manager (this stage)

Everything in `docs/01-topics.md` a module can do with no `hangard` running, so that
the library is testable alone and `hangar run` style standalone modules work.

- `hg_topic.h/.c`: the file layout to the byte (`_Static_assert` on every offset),
  wake file, directory word `.dir`; `hg_pub_open` in **standalone mode** (no
  `HANGAR_CONTROL` in the environment: the library creates the file itself, by the
  manager's steps, in `$HANGAR_TOPIC_DIR`, which must be set); `hg_publish`,
  `hg_publish_with`, `hg_publish_seq`, `hg_pub_close`; `hg_sub_open`,
  `hg_sub_open_from`, `hg_latest`, `hg_next`, `hg_read_seq`, `hg_find_stamp`,
  `hg_wait` (ready set, directory word, interrupt word, glob polling at 10 ms),
  `hg_wait_fd` (helper thread, eventfd, helper word, generation handshake), `hg_use`,
  epochs, `HG_STALE`/`HG_NO_REFS`, the stale-input check and transitive flag, the
  `reused`/`stale`/`absent` reference flags.
- `hg_type.h/.c`: type descriptors (schema text, canonical form, FNV-1a 64 hash,
  payload size), `hg_frame_type`, parsing a schema from a topic header (field list,
  offsets, sizes) for readers without generated code.
- `hg_wiring.h/.c`: the wiring text of `docs/03-manager.md` (`module`, `sub`,
  `subglob`, `pub` with `inputs=`/`sources=`, `serve`, `call`, `mirror`), read by
  `hg_init` from `HANGAR_WIRING` (an fd) or from `HANGAR_WIRING_FILE` (a path, for
  standalone runs and tests); `hg_pub_open` and `hg_sub_open` take slots, rate,
  `stale_after`, inputs and sources from it and refuse undeclared topics.
- `hg_manager_stub.h/.c` (test-only, built into the tests, not the library): what the
  manager does to topic files, so the tests can exercise it: mark died (bit 1), mark
  replaced (bit 2) + wake, bump the directory word, create a replacement file that
  continues `write_seq`.
- Tests (`lib/hg/tests/`, the root Makefile's `check`): every bullet of 01's Tests
  section that needs no manager, plus the spike's `torn` and `lost_wake` as proper
  suites, plus the publish-order guard (the test that fails with step 4 swapped).
  Run natively; under ThreadSanitizer and AddressSanitizer with no suppression.
- `hg_init`/`hg_ready`/`hg_alive`/`hg_status` exist but the module page is a no-op in
  standalone mode (no manager to hand it over).

Deferred to stage 2: the control socket (`page`, `open`), `PR_SET_DUMPABLE` at init,
calls (`hg_call_start`/`poll`/`hg_serve`, envelopes), parameters, the `.msg` generator,
the electric-eye `EEYEBUS1` adapter, the Python helper.

## Stage 2: with the manager

The control socket client; the manager itself (`hangard`) from `spike/c/src/manager.c`
and `docs/03-manager.md`; calls and parameters on topics; `hangar` CLI basics.

## Stage 3: modules

camera, vision, recorder, pairer, safety, mux, bridge, mavlink, policy: each a module
over `lib/hg`, in the order `docs/11-electric-eye.md` phase B and the version-1 path of
`docs/09-learning.md` give.
