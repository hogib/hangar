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

## Stage 2 as built: 2a and 2b

**2a, the manager and the library's side of it.** `hangard/` (GPL): manifests
(`docs/02`: strict INI, every section and key, the graph checks), `hangard.conf`, the
event loop (`epoll`, `signalfd`, one `pidfd` per module, one `timerfd`), start-up and
adoption from the registry, spawning in `docs/03`'s nine-step order (`clone3` +
`CLONE_PIDFD`, the registry pipe, cgroup when delegated, rlimits, scheduling, state
directory, journald or fallback log, capabilities, user switch last), the module page
(`HGPAGE01`), health tick and states, back-off and `OnGiveUp`, `Neutral` for actuators,
stopping and shutdown order, re-exec, the control socket (`page`, `open`, `status`,
`start`/`stop`/`restart`, `reload`, `reexec`, `clear-failsafe`), `hangar.<role>.system`
and `.modules` topics, systemd `Type=notify` and watchdog pings without libsystemd.
Library side: `hg_init` sets `PR_SET_DUMPABLE` 0 and fetches its page over the control
socket; `hg_pub_open` goes through the manager's `open` when `HANGAR_CONTROL` is set;
`hg_ready`/`hg_alive`/`hg_status` write the page. `hangar` CLI: `status`, `start`,
`stop`, `restart`, `reload`, `restart-manager`, `show`, `topics`, `echo`, `hz`, `run`.
Everything relocatable under `$HANGAR_ROOT` for tests (`etc/hangar`, `run/hangar`,
`var/lib/hangar`, the topic dir), so the suites run as an ordinary user without systemd:
user switching, scheduling and capabilities are exercised only when the test runs as
root (skipped, and said, otherwise).

**2b, calls and parameters on topics** (`docs/04`, `docs/05`): the implicit `.req`/
`.rep`/`.params` topics and their generated types, `hg_call_start`/`poll`/`hg_call`,
`hg_serve`, Allow tokens, admin calls through `hangar.<role>.admin.req`, `set_params`,
saved values, `@restart`, `hangar call` and `hangar param`.

## Stage 3 addition: the web console

A `console` module (the first half of docs/11's `pilot_console`): the manager's state as
a web page, as `hellion`'s page was for electric-eye. It reads `hangar.<role>.modules`
and `hangar.<role>.system`, lists every topic in the topic directory with its header
fields (rate declared and measured, age, writer, flags), decodes any topic's newest
message by its schema (what `hangar echo` does), and shows a module's page status line
and the recent log; start, stop and restart buttons go through the control socket as
`hangar-admin`. It serves plain HTML with a small polling script over `lib/net/http.c`
(electric-eye's fuzzed server), no framework, embedded in the binary as `hellion`'s
page was. Later it grows the pilot's controls (arm, mode, promotion) when 2b and the
safety module exist; the monitoring half needs only stage 2a.
