# The recorder

Module identifier `recorder`, binary `recorder` (unnamed as yet in
[docs/names.md](../../docs/names.md)): the recording module of
[docs/06-recorder.md](../../docs/06-recorder.md), "The recorder" and "Recording files".
It reads every topic by glob, drains the rings every 50 ms into a bounded buffer that a
writer thread empties to disk, keeps the recent past of every topic in RAM (the black
box) and writes it when something goes wrong, and records sessions, one `.hgr` file per
topic in a directory that describes itself. Read-only on every topic but its own:
`recorder.status`, `recorder.session` and `recorder.rep`.

GPL-3.0-or-later, C11 (`-std=c11 -Wall -Wextra -pedantic`, clean under gcc, clang and
`zig cc`), libc, pthreads and `lib/hg` only; one static binary for musl like the
manager. The format and the dataset code it shares with the pairer and with `hangar
dataset` are LGPL pieces of `lib/hg`.

## Layout

| File | |
|---|---|
| `recorder.c` | the process: the glob, the drain loop, sessions, the black box's triggers, the calls, `recorder.status` and `recorder.session` |
| `recorder_conf.c` | `recorder.conf`, parsed with the manifest syntax and as strictly |
| `writer.c` | the writer thread and the bounded byte ring between it and the drain loop; every file write happens here |
| `blackbox.c` | the rings in RAM, the budgets granted class by class, the hand-over of a dump |
| `recorder.h` | shared between the four |
| `recorder.module` | the manifest template (`/usr/share/hangar/modules/recorder.module`), as docs/06 writes it |
| `recorder.conf` | an example settings file, docs/06's with comments |
| `recorder.mk` | the make fragment for the recorder, the pairer, the `hangar dataset` subcommand and the suites; included by the root Makefile |
| `tests/` | the four suites (`rec_hgr`, `rec_dataset`, `rec_recorder`, `rec_pairer`), the toy chain `tests/modules/prov_chain.c`, `rec_test.h` |
| `../../lib/hg/hg_record.[ch]` | the `.hgr` format: header and record layouts, writer, reader (LGPL) |
| `../../lib/hg/hg_dataset.[ch]` | dataset specs, the row type, the join (LGPL; [modules/pairer/README.md](../pairer/README.md)) |
| `../../lib/hg/hg_ini.[ch]` | the manifest syntax as a strict walker (LGPL) |
| `../../hangar/hangar_dataset.c` | `hangar dataset build\|check\|schema\|pairs` |
| `../../python/hangar/record.py`, `dataset.py` | the Python readers |
| `../../msg/recorder_start.msg`, `recorder_session.msg`, `recorder_clock.msg` | the request of `start`, the session topic, the records of `hangar.clock.hgr` (`recorder_status.msg` existed) |

## What it does

**Subscribing.** One glob subscription, `*`, opened from the oldest message still in
each ring, so a topic that appears after the recorder started is recorded from its
first messages and not from the newest alone. Every 50 ms the drain loop rescans the
directory word, attaches new matches (settling their policy, their black-box budget
and the header their files will start with, once, never per message), and reads each
ring in order with `hg_next` into one scratch buffer the size of the largest payload.
A ring too shallow for the drain is warned of at attach (`fast.a: 2 slots at 200 Hz
hold 5 ms, under the 60 ms the recorder needs`). `hg_wait` returns within 10 ms because
of the glob; the calls are served on every return, the status is published on change
and once a second, and `hg_alive()` is called on that tick.

**Records.** Each message becomes a 128-byte record header and, unless the policy is
`meta`, its payload, as `lib/hg/hg_record.h` lays them out (the header comment there is
the byte-exact description; the Python reader is written to it). A loss the reader saw
(`info.gap`), or a record the writer's buffer could not take, is carried by the next
record of that topic: bit 16 and `gap`. A record that follows seqs a rate-limited
mirror skipped keeps the slot's bit 1 and carries the skipped count in `gap`. The
policies `every:N` and `rate:<Hz>` keep the messages whose seq is a multiple of N,
whenever the session started; a message left out by policy is not a gap.

**The writer thread** is handed messages through a byte ring of `WriteBuffer` bytes
(open this topic's file with this header; append this record; begin a session in this
directory; write this text file; close every file; write this dump). The drain loop
never blocks on it: a full ring drops the record, and the next one carries the gap.
The writer does every file operation: `mkdir` of the session's subdirectories, the
copies of the manifests and the message types, `session.ini`, the `.hgr` files
(`O_EXCL`, mode 0640, one `writev` per record), the black-box dumps.

**Sessions.** `start` (with a note), `stop` and `dump` are served as docs/06 lists
them; `AutoStart=boot` starts one at start-up, `AutoStart=armed` starts one when
`safety.state` says armed (`state` 1) and stops it when it no longer does. A session is
`<Dir>/<YYYYmmdd-HHMMSS>-<host>/` (mode 2770, group `hangar-rec` where the group
exists), with `session.ini`, `manifests/` (a copy of `$HANGAR_ROOT/etc/hangar/modules/*.module`),
`msg/` (a copy of `$HANGAR_ROOT/usr/share/hangar/msg/*.msg`), `video/`, `weights/`,
`hangar.clock.hgr` (once a second: the monotonic and the wall clock) and one
`<topic>.hgr` per recorded topic, `<topic>+<k>.hgr` for each later epoch of a mirrored
topic. At the start of a session the newest message of every event topic is written
first, so the state at the start is known without waiting for a change; the message
that started a session by `AutoStart` is its first record, the one that stopped it its
last. A session stops when the free space falls under `MinFree`, and the status line
says so. `recorder.session` is published when a session starts and stops, referencing
the request that caused it when a call did.

**The black box.** Each attached topic has a ring of records in RAM with a budget:
`BlackBox` seconds at its declared rate, or its `Slots` for an event topic. Budgets
are granted class by class (`high`, `normal`, `low`) within `BlackBoxMemory`; the first
class that does not fit whole shares what is left in proportion, the classes after it
get nothing. The grant is recomputed when a topic appears (the one allocation on that
path; the newest entries of a ring that shrinks are kept). A dump hands the rings to
the writer whole, gives the live rings fresh buffers, and the writer writes
`<Dir>/blackbox-<time>-<n>/` in the session layout with a `session.ini` naming the
reason and each topic's reach. Dumps happen on `dump`, when `hangar.<role>.system`
enters failsafe (`state` 2), and when `hangar.<role>.modules` shows a module going from
starting, ready or unhealthy to backoff or failed (a stop by request goes through
`stopping` and is not an exit); at most one dump every 2 s, and none while one is being
written.

## `recorder.conf`

The manifest syntax, parsed as strictly: an unknown section or key, a repeated key or a
bad value stops the recorder with exit status 78, naming the line. `[Recorder]` once,
`[Record <glob>]` any number of times, matched in order.

| `[Recorder]` key | Default | |
|---|---|---|
| `AutoStart` | `armed` | `boot`, `armed`, `manual` |
| `Dir` | `/var/lib/hangar/recordings` (under `$HANGAR_ROOT`) | the sessions' directory |
| `MinFree` | `1GB` | a session stops under this much free space |
| `BlackBox` | `60s` | the black box's length; `0` turns it off |
| `BlackBoxMemory` | `32MB` | its largest size in RAM |
| `WriteBuffer` | `16MB` | the ring between the drain loop and the writer thread (not in docs/06; at least 64KB) |

| `[Record <glob>]` key | Default | |
|---|---|---|
| `Policy` | `full`; `meta` for a frame topic | `full`, `every:N`, `rate:<Hz>`, `meta`, `off` |
| `BlackBox` | `off` for `dataset.*`, `low` for frame topics, `high` for event topics, `normal` otherwise | the class |

A frame topic is one whose schema has the fields `width`, `height`, `fourcc`, `stride`,
`offset` and `data`.

## Running it

Under the manager: install `recorder` as `/usr/lib/hangar/recorder`, symlink
`recorder.module` into `/etc/hangar/modules/`, write `/etc/hangar/recorder.conf`,
`hangar reload`. The manager makes the default `Dir`; the recorder refuses to start
(78) when `Dir` is not a directory of mode 2770, or of group `hangar-rec` where that
group exists.

By hand, as `hangard/README.md` "Running it standalone" sets a root up:

```sh
R=/dev/shm/hangar-dev
mkdir -p $R/var/lib/hangar/recordings && chmod 2770 $R/var/lib/hangar/recordings
printf '[Recorder]\nAutoStart=boot\n' > $R/etc/hangar/recorder.conf
HANGAR_ROOT=$R HANGAR_TOPIC_DIR=$R/dev/shm/hangar builddir/recorder $R/etc/hangar/recorder.conf
```

Unwired (no manifest, no `HANGAR_WIRING_FILE`), the calls are not served, which the log
says once; `AutoStart` works. `hangar run recorder` wires it from its manifest. Its
output: one line per topic as it attaches, one per session start and stop, one per
dump, the warnings.

## Tests

`make check-recording` (part of `check`) runs four suites, each under 30 s, as an
ordinary user under a scratch `$HANGAR_ROOT` on tmpfs
(`/dev/shm/hangar-test-<uid>/rec-<suite>-<pid>`; `$HANGAR_TEST_BASE` overrides,
`REC_KEEP` keeps the root). The root Makefile already had a `recorder` suite, electric-eye's
MP4 recorder, so these are `rec_*`.

| Suite | |
|---|---|
| `rec_hgr` | the header and record layouts to the byte; a round trip of depth messages read from a topic with a gap and a decimation; a `meta` file; a cut-off file read up to its last whole record; the searches by seq and by stamp (publish time when the stamp is 0); a file that is not a recording refused |
| `rec_dataset` | the dataset library ([modules/pairer/README.md](../pairer/README.md)) |
| `rec_recorder` | the recorder as a process, wired by a file, the toy chain publishing, the suite as `tester` making its calls: sessions by call with their layout, `session.ini`, `hangar.clock.hgr`, every message of a rated topic in order, `every:2` and `meta` from the first matching section, the same seqs kept by `every:2` in two sessions, the newest event message at a session's start, `tester.req` and `recorder.rep` recorded with the reply naming its request, a mirrored topic's two epochs in two files with the decimation flag kept; a 2-slot ring at 200 Hz warned of and its losses recorded as gaps that add up; the black box dumped on `dump`, on failsafe and on a module's exit, within 64 KB with the event topics whole and the rated ones sharing; `AutoStart=armed`; exit 78 for a `Dir` of the wrong mode and for four bad `recorder.conf` files |
| `rec_pairer` | the pairer and the build on one run ([modules/pairer/README.md](../pairer/README.md)) |

`make check-tsan-recording` and `check-asan-recording` (run by `check-tsan` and
`check-asan`) rebuild `rec_hgr` and `rec_dataset`, which hold the library pieces, under
the sanitizers. `check-recording-py` runs `python/tests/test_record.py` and
`test_dataset.py` against the fixtures the two library suites write with
`HGR_FIXTURE_DIR` set (`python/tests/fixtures/`, checked in: `depth.hgr`,
`chain.dataset`, `dataset.chain.hgr`).

## Deviations from docs/06, with reasons

Where the text could not be followed to the letter, or left something the code had to
decide. Nothing here changes behaviour silently.

1. **The glob is opened from the oldest message.** docs/06 says the recorder "takes
   topics as they appear" and docs/01 that a fresh subscription starts at the newest
   message. A topic that appears would then be recorded from one message on, its first
   ones never kept; opened `HG_OLDEST`, a new match is recorded from the oldest message
   still in its ring. Topics present at start are read the same way, which fills the
   black box at once.
2. **`Dir`'s group is checked only where the group exists.** docs/06 refuses a `Dir`
   that is not group `hangar-rec` with mode 2770. On a machine without the group (the
   suites run as an ordinary user) the mode is checked and the group is not, and the
   log says so once. A missing `Dir` is created with mode 2770 rather than refused, so
   a hand-run recorder has somewhere to write.
3. **`recorder.status` has one black-box reach, the least.** `msg/recorder_status.msg`
   existed with `blackbox_ns` as "the least over its topics" and no per-topic field (the
   `.msg` grammar has no variable-length lists); the per-topic reach is in every dump's
   `session.ini` under `[BlackBox]`.
4. **A record carrying both lost and skipped messages reports the loss.** The record's
   one `gap` word cannot hold both; bit 16 wins and `gap` is the loss, the skipped count
   of that record goes into the recorder's statistics alone. The case needs a
   rate-limited mirror and a loss between two drains of the same topic.
5. **`WriteBuffer`** sizes the bounded buffer between the drain loop and the writer,
   which docs/06 names but does not size. 16 MB holds about 300 ms of a 1280x720 I422
   frame topic at 30 Hz recorded `full`, or a second of everything else.
6. **`hangar.clock.hgr`'s offset is 0.** docs/06 wants "the time daemon's estimated
   offset"; there is no interface to one yet, so the field (`offset_ns` of
   `recorder_clock`) is 0 and the monotonic and wall clocks are recorded as read.
7. **`session.ini` carries no other side.** The other side's session id and boot id
   come from the bridge, which does not exist. Its keys are `Id`, `BootId`, `Host`,
   `Recorder`, `StartedBy` (`call:<caller>`, `autostart:boot`, `autostart:armed`),
   `Note`, the start and stop times on both clocks (`StartedMonotonicNs`,
   `StartedRealNs`, `StartedUtc`, and `Stopped...`), `StoppedBy` (the caller,
   `autostart:disarmed`, `min-free`, `shutdown`), `Records`, `Bytes`, `WriteErrors`; a
   dump's has `Kind=blackbox`, `Reason`, the dump time, `Session` (the session running,
   if any) and `[BlackBox]` with one `<topic>=<reach ns>` line per topic.
8. **The message types copied into `msg/`** are `$HANGAR_ROOT/usr/share/hangar/msg/*.msg`,
   the manager's default `MsgDir`; a `MsgDir` set otherwise in `hangard.conf` is not
   read. Every recorded topic's schema is in its file's header regardless.
9. **The triggers read fields by name.** `safety.state` (its `state`, 1 for armed),
   `hangar.<role>.system` (`state` 2 for failsafe) and `hangar.<role>.modules` (`name`,
   `state`) are found among the glob's matches by name and their fields by the schema
   in the header; a topic of that name with another layout is ignored for the trigger
   and recorded like any other.
10. **Dumps are debounced** at one every 2 s and one at a time: an incident that
    trips several triggers (a module exit that puts the system in failsafe) makes one
    dump, and a dump that would race the previous one's write is logged and skipped. A
    dump allocates (the fresh rings), once per incident.
11. **The recorder records its own topics.** `[Subscribe *]` matches `recorder.status`,
    `recorder.session` and `recorder.rep`, so a session holds its own status history and
    the replies to the calls that drove it, as docs/06 wants of every call; the start
    message of `recorder.session` references the `start` request.
12. **Epochs** are tested with `hg_pub_open_mirror` standing in for the bridge: a
    replaced file with another `origin_boot` continues in `<topic>+<k>.hgr`, and the
    black box ring of that topic starts over, so one file never holds two numberings.
13. **The calls are served only when wired.** Without a wiring (no manifest) the
    library has no `serve` lines; the recorder logs that and runs with `AutoStart`.
14. **No suite runs it under the manager.** The suites wire the recorder by a file and
    make the calls from the suite's own process; `recorder.module` is docs/06's template
    and the manifest parser accepts it (the console suite is the model for a suite
    under `hangard`). It has run under `hangard` by hand, as
    `profiles/sim-vehicle/recorder.module` and `recorder.conf` enable it for the
    development demo: the manager accepts the manifest, `AutoStart=armed` follows
    `hangar call safety.arm` and `safety.disarm`, the calls work through
    `hangar call`, the sessions and a dump read back, and the shutdown is clean;
    `profiles/sim-vehicle/RECORDING.md` has the run, command by command. Nothing
    in the module needed a change for it.

## Until the bridge

- A mirrored topic's new epoch, and `origin_boot` in the header, exercised only with
  `hg_pub_open_mirror` in the suite.
- `session.ini`'s other-side session id and boot id (docs/06, docs/08).
- The rate-limited mirror's skipped messages (deviation 4) seen only as the suite
  publishes them with `hg_publish_seq`.
- `topside_recorder`: the same binary under another identifier; its topics and calls
  take the module's name from the wiring, so nothing in the code is tied to
  `recorder` but the log prefix and the standalone defaults.
