# hangard, stage 2a: the manager and the library's side of it

`hangard` is the module manager of `docs/03-manager.md`, over the manifests of
`docs/02-manifest.md`; `hangar` is the command of `docs/12-tools.md`, the stage-2a
subset. Both are GPL-3.0-or-later, C11, nothing beyond libc and the kernel, and build
static for musl (`zig cc -target aarch64-linux-musl`). The library side (the control
socket client, the module page, `hg_pub_open` through the manager) is in `lib/hg`
and stays LGPL. The plan is `lib/hg/PLAN.md`, "Stage 2 as built".

## Layout

| File | |
|---|---|
| `hangard.c` | the event loop (`epoll`: `signalfd`, one `timerfd`, one `pidfd` per module, the control socket), start-up steps 1 to 6, adoption from the registry, reload, re-exec, shutdown, the manager's own topics, the systemd notify socket |
| `conf.c` | `hangard.conf` and the fixed paths, relocated under `$HANGAR_ROOT` |
| `manifest.c/.h` | the strict INI parser, every section and key of docs/02, the single-manifest rules, the implicit `.req`/`.rep`/`.params` topics and subscriptions, the generated `req:`/`rep:`/`params:` types, the graph checks |
| `wiring.c/.h` | the wiring text of docs/03, into a buffer or a sealed memfd |
| `spawn.c` | `clone3` + `CLONE_PIDFD`, the registry entry and Neutral record before the child is released, the child's ten steps, the module page, Neutral commands, signals to a module's group or cgroup |
| `registry.c` | the text registry, `/proc`, users and groups from `/etc/passwd` and `/etc/group` read by hand |
| `topics.c` | topic files created on a module's behalf by docs/01 "Opening", the dead-writer flag, the directory word, the second-writer check |
| `health.c` | states and transitions, the health tick, back-off, `Burst`, `OnGiveUp`, exits, stops |
| `control.c` | the control socket server |
| `msg/hangar_system.msg`, `msg/hangar_modules.msg` | the types of `hangar.<role>.system` and `.modules`; `make msg` generates them into `msg/` with the others |
| `hangard.mk` | the build rules, included by the root Makefile |
| `hangard.service` | the systemd unit of docs/03 |
| `tests/` | four suites and the toy modules (below) |
| `../hangar/hangar.c` | the command |
| `../lib/hg/hg_control.h/.c` | the control socket's wire format and client |

## Running it standalone, as an ordinary user

Every fixed path moves under `$HANGAR_ROOT` when it is set: `/etc/hangar`,
`/run/hangar`, `/var/lib/hangar`, `/usr/share/hangar/msg`, `/dev/shm/hangar` and the
journald socket `/run/systemd/journal/stdout`. Unset, the real paths are used.

```sh
R=/dev/shm/hangar-dev
mkdir -p $R/etc/hangar/modules $R/usr/share/hangar/msg
cp msg/*.msg $R/usr/share/hangar/msg/
printf 'Role=vehicle\n' > $R/etc/hangar/hangard.conf
cat > $R/etc/hangar/modules/talker.module <<EOF
[Module]
Exec=$PWD/builddir/tests-debugoptimized/hangard-modules/talker --topic test.depth --rate 50
[Publish test.depth]
Type=depth
Rate=50
EOF
HANGAR_ROOT=$R builddir/hangard &
HANGAR_ROOT=$R builddir/hangar status
HANGAR_ROOT=$R builddir/hangar topics
HANGAR_ROOT=$R builddir/hangar echo test.depth --count 3
HANGAR_ROOT=$R builddir/hangar hz test.depth --seconds 2
HANGAR_ROOT=$R builddir/hangar show talker
HANGAR_ROOT=$R builddir/hangar run talker      # the module alone, private topic dir
```

Not running as root, the manager skips the steps that need it and says so in its log,
once per start, naming the step: the cgroup move (step 2), the scheduling policy and
priority (step 4), capabilities and the user switch (steps 7 to 9). Everything else,
the registry, the Neutral records, the module pages, the wiring memfd, journald or the
fallback log, the topic files, happens as documented. As root under systemd
(`hangard.service`) every step runs; the suites exercise the privileged ones only
`if (geteuid() == 0)` and report them skipped otherwise.

`hangard.conf` is flat `Key=Value` with the keys of docs/03 "Running it" (`Role`
required; `PrivilegedModule`, `ModuleDir`, `MsgDir`, `TopicDir`, `BridgeConf`,
`DatasetDir`, `RecordingDir`, `HealthTick`, `MaxModules`, `LogFallbackDir`,
`LogFallbackCap`); an unknown or repeated key refuses the file with its line.
`hangard -c <file>` names another file.

## The control socket

`<run>/hangar/control`, `SOCK_SEQPACKET`, mode 0660, group `hangar`. One request per
packet, one reply per request, text; the connection is closed after the reply. Callers
are identified by `SO_PEERCRED` and `SO_PEERGROUPS`. The same text is in
`lib/hg/hg_control.h`, which is the client.

| Request | From | Reply |
|---|---|---|
| `page` | a module's main process (`hg_init`) | `ok\n` + the page descriptor (`SCM_RIGHTS`); refused if the pid is not a module's main process, the module is `Legacy`, or `TracerPid` in `/proc/<pid>/status` is not 0 |
| `open <topic> hash=<16 hex> payload=<bytes> schema=<len>\n<schema bytes>` | a module's main process (`hg_pub_open`) | `ok\n` + the writable topic descriptor. Checked: the topic is one of the module's declared or implicit outputs; the schema parses and hashes to the hash given; the hash is the declared type's (`frame`: any frame schema); no other live process holds the file for writing. Then the file is created by docs/01 "Opening": temporary file, `posix_fallocate`, header, wake file (`O_CREAT`, once per boot), bit 2 and a wake on the old file, rename, directory word bumped and woken. A `boot=<32 hex> first=<seq>` pair (the bridge's mirrored topics) is parsed and refused: no bridge in this stage |
| `restart-self` | a module's main process | `ok\n`: a stop and a start that counts no failure |
| `status` | group `hangar` | `ok\n`, then `system <nominal\|degraded\|failsafe> <since_ns> <reason>` and one `module <name> <state> <pid> <failures> <last_exit> <since_ns> <status line>` per module; `last_exit` is `exit:<n>`, `signal:<n>`, `unknown` or `-` |
| `wiring <module>` | group `hangar` | `ok\n` then the wiring text as handed to the module (for `hangar show`) |
| `start`, `stop`, `restart <module>` | `hangar-admin` | `ok\n` or `err <reason>\n`. A start or restart by hand clears the module's failure history |
| `reload` | `hangar-admin` | as `SIGHUP`: manifests re-read; unchanged modules untouched, changed restarted, removed stopped, new started; a refused graph changes nothing and the reply says why |
| `reexec` | `hangar-admin` | `ok\n`, then the manager re-executes `/proc/self/exe` in place |
| `clear-failsafe` | `hangar-admin`, or the module `safety` | leave failsafe; refused, naming the module, while a module given up with `OnGiveUp=failsafe` is `failed` |

`hangar-admin` is the group of that name, uid 0, or the manager's own uid (so that a
manager run by hand, and the suites, can be driven without the group). Every reply
starts with `ok` or `err`; a refusal is also logged.

**Re-exec hand-off.** The state goes to the new image as text in a memfd named by
`$HANGAR_REEXEC_FD` (`hangar-reexec 1`: the system state, the control socket's
descriptor number, and per module its pid, pidfd, start time, state, since, failures,
delay, restart time, instance, ready time, exit, give-up and adoption flags, both
hashes, the stop deadline and the failure times); the pidfds and the control socket
have their close-on-exec flag cleared. Modules stay children; the manager's own topics
are opened again by the new image, which continues their sequence numbers (below).

## What the library does under a manager

With `HANGAR_CONTROL` set (the manager sets it for every module), `hg_init` first
makes the process non-dumpable (`prctl(PR_SET_DUMPABLE, 0)`), reads the wiring from
the sealed memfd `HANGAR_WIRING`, then sends `page` and maps the page; it fails with
`hg_init_error()` saying why if the manager refuses. `hg_ready`, `hg_alive` and
`hg_status` write the page (`ready_instance` set to the page's `instance`,
`alive_ns` atomically, the 128-byte status line); the library also sets ready by
itself once every rated output of the wiring has published once. `hg_pub_open` sends
`open` and maps the descriptor it gets back instead of creating the file; a refusal
is `EACCES` (not an output), `EPROTOTYPE` (type) or `EBUSY` (another writer). While
the manager is not listening (it is restarting) both requests wait and retry, as
docs/03 says. Without `HANGAR_CONTROL` nothing changes from stage 1 except the
`PR_SET_DUMPABLE`.

## Tests

`make check-hangard` (part of `check`) runs four suites, each under 30 s, as an
ordinary user under a `$HANGAR_ROOT` of its own on tmpfs
(`/dev/shm/hangar-test-<uid>/hangard-<suite>-<pid>`, `$HANGAR_TEST_BASE` overrides):

| Suite | |
|---|---|
| `hangard_manifest` | every key parsed, every parse refusal with its file and line, every graph refusal of docs/02 staged alone, the implicit topics and subscriptions, the wiring round-tripped through `hg_wiring_parse` and a sealed memfd |
| `hangard_run` | a manager with a talker and a listener: ready via the page, `status`/`stop`/`start`/`restart`/`wiring`, `page` refused to a non-module, `open` refused for an undeclared topic, a wrong type and a second writer, the registry's contents and modes, the journald stream protocol against a stand-in socket, the fallback log, `hangar status\|topics\|echo\|hz\|show\|run`, shutdown |
| `hangard_health` | staleness, back-off doubling, `ResetAfter`, `Burst` → `failed` and `degrade` on `hangar.vehicle.system`, exit 78, never ready after `StartGrace`, the watchdog, an actuator's `Neutral` run on `SIGKILL` and at shutdown, failsafe on a give-up and on a failing `Neutral`, `clear-failsafe`, the shutdown order |
| `hangard_adopt` | the manager killed with `SIGKILL` under load, adoption, a module killed meanwhile flagged and restarted, a registry pid of another process treated as dead, the system state kept, reload with a changed, a removed and an added manifest and a refused graph, `SIGHUP`, re-exec keeping the modules as children and the manager's sequence numbers, the idle manager's wake-ups |

The toy modules (`tests/modules/`, each a few dozen lines over lib/hg): `talker`
(a rated depth topic; `--stop-after`, `--exit-after`), `listener` (`hg_ready`,
`hg_alive`), `crasher`, `neverready`, `eventer` (`--stop-alive-after`, `--exit-78`),
`actuator` and `neutral` (the Neutral command: appends a line to a file;
`--fail`, `--hang`). `check-tsan-hangard` and `check-asan-hangard` (run by
`check-tsan`/`check-asan`) rebuild the manifest suite under the sanitizers.

## What stage 2a leaves to 2b and later

- Calls and parameters (`docs/04`, `docs/05`): `hg_call_start`/`poll`/`hg_call`,
  `hg_serve`, the envelopes, `set_params`, saved values, `@restart`; the control
  requests `call` and `param-write`; publishing on `hangar.<role>.admin.req` (the
  topic exists, nothing is published on it yet); `hangar call`, `hangar param`. The
  wiring already carries `serve` and `call` lines with the generated types, and the
  `.req`/`.rep`/`.params` outputs with 16 slots, so a 2b module needs no wiring change.
  Not emitted yet: the implicit `serve set_params` line of a module with `[Params]`
  (its request type `set_params:<type>` is 2b's); the `.rep` payload already allows for
  `set_params_rep`.
- The bridge (`docs/08`): `bridge.conf`, `mirror` lines, `subglob *.req` for methods
  the bridge carries, `remote=1` calls, mirrored opens (`boot=`/`first=`). A manifest
  with `[Bridge]` is refused, saying so.
- Dataset row types (`Type=dataset:<name>`, `docs/06`): refused, saying so.
- The policy-module checks (`hangar-policy` under `policy_<Name>`, the three outputs;
  `docs/09`).
- `hangar check`, `top`, `graph`, `bench`; `hangar echo` of a `.req`/`.rep` body by its
  type hash (the envelope decodes like any schema).
- Running the suites as root (user switching, `SCHED_FIFO`, capabilities, cgroups) on
  a machine or VM; they are skipped and reported here.

## Deviations from docs/02 and docs/03, with reasons

Where the text could not be followed to the letter, or left something the code had
to decide. Nothing here changes behaviour silently.

1. **The registry's module lines carry two more fields**: the FNV-1a 64 of the
   manifest's text and of the wiring text the running instance was started with
   (`<name> <pid> <starttime> <state> <failures> <delay> <manifest> <wiring>`). docs/03
   gives six fields. A reload after an adoption or a re-exec must know what the running
   instance was started with to leave an unchanged module untouched and restart a
   changed one; the format stays text, and a reader of the six fields ignores the rest.
2. **A `stopping` state.** docs/03's table has no name for "SIGTERM sent, waiting for
   the exit". A module stopped on request, restarted on request, or stopped at
   shutdown shows `stopping` in `status`, the registry and `hangar.<role>.modules`
   (state 7); an unhealthy module keeps `unhealthy` while it is stopped, as the table
   says. A `stopping` module adopted from the registry has its stop finished at once.
3. **"Refused graph" on reload means any refused manifest.** docs/02 says a reload
   that produces a refused graph changes nothing, and that at load a refused
   manifest leaves the others loading. Both are kept: at start every accepted module
   runs and the refused are listed `refused` with their reason; a reload is refused
   whole, naming the first reason, if any manifest in the new set is refused.
4. **The second-writer check** refuses an open when the existing file is not closed,
   dead or replaced and the process its header names is alive and not the asker: a
   module's main process, the manager, or any other live process (a helper that kept
   the descriptor). A dead writer's file carries bit 1, set by the manager at its exit
   or at start-up, so a reused pid is met by a flagged file, not refused.
5. **The manager's own uid counts as `hangar-admin`**, beside uid 0 and the group, so
   that a manager run by hand, and the suites, can be driven without creating groups.
   In production the manager is root, and uid 0 already counts.
6. **`hangar.<role>.modules` is one message per module**, with `index` and `count`
   fields to assemble a round: the `.msg` grammar has no nested types or variable
   lengths. The topic is declared at 1 Hz (a round a second) with 256 slots.
7. **The idle manager wakes once a second, not never.** docs/03 "Event loop" has the
   manager publish its topics at 1 Hz when nothing else needs it; the adoption suite
   measures 3 context switches in 3 s and asserts at most 4. Zero is not reachable
   under docs/03's own text.
8. **Re-exec reopens the manager's own topics** instead of passing their descriptors:
   lib/hg has no "adopt an open writer" entry point, and reopening is what every
   writer restart does anyway (bit 2 on the old file, `write_seq` continued, readers
   reopen without a gap). The pidfds and the control socket are passed as docs/03
   says; the wiring memfds are regenerated; the pages are files.
9. **Allow tokens naming this machine's own role are refused** (`vehicle:safety` on the
   vehicle): docs/04 defines `<role>:<module>` for the other side; a local caller is
   written bare. docs/02's "an unknown token refused" is read to include it.
10. **`[Bridge]`, `Type=dataset:<name>` and the policy-module checks are refusals**
    with a reason that names the stage, not silent acceptance; see the list above.
11. **The health tick** runs while any running module is `starting`, has a rated
    output or a `Watchdog`, as docs/03 says; a `Legacy` module is `ready` from its
    start and never ticked. An adopted module's `StartGrace` and `ResetAfter` count
    from the adoption, since the old instance's times are not in the registry; the
    adopted actuator whose output is already stale is caught by the first tick, as
    required, because `stale_after + StaleGrace` is measured from the topic's own
    `last_publish_ns`.
12. **A `Legacy` module** gets `HANGAR_MODULE`, `HANGAR_TOPIC_DIR` and `HANGAR_ROOT` but
    no `HANGAR_WIRING` and no `HANGAR_CONTROL`: it has no wiring, no page and asks for
    nothing.
13. **The journald socket relocates under `$HANGAR_ROOT`** like the other `/run`
    paths, so that the suites' modules never write into the real journal and the
    stream protocol can be checked against a stand-in socket.
14. **`hangard.conf` is flat `Key=Value`.** docs/03 gives a key table and no section;
    the manifest's comment and blank rules apply.
15. **Capabilities accepted** are the manager's own set (the unit's bounding set),
    as docs/02 says; `CAP_SYS_NICE` is the only one a shipped module uses.
16. **Users and groups** are read from `/etc/passwd` and `/etc/group` only when the
    switch will be made (as root). Not root, an unknown `User=` is not an error, so the
    actuator manifests of the suites (which must name a dedicated user) run.
17. **A module with no manifest left after a reload is dropped from `status` once it
    has stopped**; docs/02 says only that it is stopped.
18. **`hangar run`** takes `--msgdir` and `--role` beside the module (or a manifest
    path), reads `ModuleDir`/`MsgDir`/`Role` from `hangard.conf` when present, and puts
    the private topic directory under `$XDG_RUNTIME_DIR/hangar-run/<module>`
    (`/dev/shm/hangar-run-<uid>/<module>` without it), writing the wiring beside it as
    `.wiring`.
19. **The generated files' header comment** says `msg/hangar_system.msg` although the
    source is `hangard/msg/hangar_system.msg`: the generator prints the stem's
    canonical place. The `.msg` sources live here so that the manager's types are the
    manager's; `make msg` regenerates both.
