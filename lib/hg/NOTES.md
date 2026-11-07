# lib/hg, stage 1: notes

What was built, where it departs from `docs/01-topics.md` and why, and what
stage 1 leaves out. The plan is `PLAN.md`; the contract is `docs/01-topics.md`
with the wiring of `docs/03-manager.md` and the call topics of `docs/04-calls.md`.

## Files

| File | |
|---|---|
| `hg_sys.h` | futex, `futex_waitv`, memfd flags and seals declared by hand; compiles against glibc and musl |
| `hg_type.h/.c` | the type descriptor of `hg_type_abi.md`, the schema parser (layout, canonical form, FNV-1a 64), `hg_frame_type` |
| `hg_wiring.h/.c` | the wiring text: `module`, `sub`, `subglob`, `pub` (`inputs=`, `sources=`), `serve`, `call`, `mirror`; durations, rates, globs |
| `hg_topic.h/.c` | the topic file, writer, reader, provenance, `hg_wait`, `hg_wait_fd`, `hg_init` and the module page |
| `hg_control.h/.c` | the control socket's wire format and client (stage 2a) |
| `hg_call.h/.c` | calls over topics (stage 2b, docs/04): the envelopes, `hg_call_start`/`poll`/`hg_call`, `hg_serve`, caller tokens and Allow lists |
| `hg_params.h/.c` | parameters (stage 2b, docs/05): the grammar of values, ranges and the saved file; `hg_params_open`, the `<module>.params` topic, `set_params` |
| `hg_manager_stub.h/.c` | test-only: what the manager does to topic files, plus the publish protocol taken apart for the order guard |
| `tests/` | nine suites, one per area, on `lib/common/test_harness.h` |

The spike's deviations 1 (exchange before increment), 4 (`write_seq` seq_cst),
5 ("flags changed" readiness), 7 (resolved as below) and 11 (`HG_ERROR`, length
checks, validated header copy) are the design and are implemented as such;
deviation 6's polling is gone, replaced by the directory word; deviation 3's
`hg_wait_interrupt` stays.

## Design deviations

Where the text of `docs/01-topics.md` could not be followed to the letter, or
left something open that the code had to decide. Each is the closest correct
thing I could find, with the reason; none changes behaviour silently.

1. **The interrupt word takes a futex, and the count still fits.** `hg_wait`
   sleeps on a process-private word so `hg_wait_interrupt` (from a signal
   handler, or another thread) ends a sleep that a signal alone would not end
   between the caller's check and the sleep (spike deviation 3). docs/01 counts
   128 futexes as 127 topics plus the directory word, with no room for it. It
   fits anyway: the directory word is only in the set while some subscription is
   absent, and an absent subscription has no futex, so 127 attached topics plus
   the interrupt word are 128, and *k* absent ones leave *k* places for the
   directory word and the interrupt. `HG_MAX_WAIT` is 127 as the design says, and
   `HG_MAX_WAIT_FD` 126 (the helper word; the helper does not need the interrupt).

2. **A fresh subscription returns the newest message once.** docs/01 says a
   fresh subscription "starts `hg_next` at the newest message: nothing published
   before the open is returned", and its test says the first `hg_next` "starts at
   the newest message, not at the oldest safe slot". Taken as "returns nothing
   that exists", a dashboard or a controller opening a 1 Hz topic would see
   nothing for up to a second. Implemented as: the first `hg_next` or `hg_latest`
   returns the newest message if there is one, with no gap for anything before
   it, and then only what is published later. `HG_OLDEST` starts at `W − K + 2`
   as written. This is the one reading of "starts at the newest message" under
   which `hg_latest` on a fresh subscription is useful.

3. **Payload length admits the C struct's own size.** The payload is the
   type's size rounded up to 8, but `sizeof` of the generated struct is the
   unrounded size (`struct depth` is 12 bytes in a 16-byte payload). `hg_publish`
   accepts a length between `payload_bytes − 7` and `payload_bytes` and zero-pads
   the tail word; readers' buffers may be as short as `payload_bytes − 7` and get
   the bytes they have room for. Anything else is `EINVAL`/`EMSGSIZE` as in the
   spike.

4. **A pending replacement gives up after one second.** docs/01 says a reader of
   a file with bit 2 reopens the path; between the manager's flagging and its
   rename the path still holds the old inode. The reader waits on the directory
   word (not a poll, as the spike did) and reopens when it changes. If no new
   file has appeared a second after the flag, `hg_next`/`hg_latest` report
   `HG_CLOSED` (kept from the spike): a replacing writer that died between the
   flag and the rename would otherwise leave its readers reporting `HG_NONE`
   forever. The reader keeps waiting on the directory word and switches as soon
   as a file appears.

5. **`.dir` and the wake files in standalone mode are the library's.** With no
   manager, `hg_pub_open` creates the topic's wake file and `.dir` (`O_CREAT`,
   never truncated, never renamed) and bumps the word after the rename; a
   subscriber creates `.dir` too if it is missing, since it must wait on it. Under
   a manager the module maps the existing `.dir` read-only when it may not write
   it. docs/01 gives both files to the manager alone; in standalone mode there is
   no one else.

6. **`hg_use` holds until the next `hg_use`.** docs/01 says `hg_use` "makes
   that message the reference, instead of the most recent one" and no more. A
   one-shot override would be lost by a module publishing two outputs from one
   input; an override cleared by the next read would be lost by the very
   pipelining it is for. So it holds until the module calls `hg_use` again (with
   the most recent `info` to go back to "most recent"), or the topic starts a new
   epoch.

7. **A `<role>:<topic>` input the module does not supply is recorded as seq 0.**
   docs/01 says the module "gives the reference for itself with
   `hg_publish_with`"; when it does not, the entry is still one of the message's
   `Inputs`, so it is recorded with the topic id of the name after the colon,
   seq 0 and no flags, and never checked, rather than dropped.

8. **A reference to a declared `Inputs` topic that is open in this process as
   an output, not a subscription, references the output.** The registry resolves
   a link by name among open subscriptions first, then among this module's own
   outputs ("a link may also be to another of the module's own outputs").

9. **`hg_wait` returns `(unsigned)-1` on a bad argument** (`n` of 0 or over
   `HG_MAX_WAIT`): the design gives it an unsigned ready set and no error value.
   `HG_READY_INTERRUPT` (2) is the interrupt's and a signal's bit, beside
   `HG_READY_TOPICS` (1). `hg_wait_ready` is the same call with a per-topic
   `ready[]` array, which the tests of "a request and a data topic reported in one
   pass" need; `hg_wait` is `hg_wait_ready(..., NULL)`.

10. **An absent topic that appears is news once; one present at the open is
    not.** docs/01 lists "an absent topic that appeared" among news. A topic
    present when `hg_sub_open` ran is not reported as having appeared (its newest
    message, if any, is news by itself under deviation 2), so a module opening an
    empty topic does not get a wake-up with nothing behind it.

11. **`hg_wait_fd` signals the eventfd on every pass**, including the pass a set
    change or its own start causes: the helper cannot know what the module has
    read (that state is the module thread's), so it signals whenever its snapshot
    was taken and lets the module find nothing. docs/01 asks that no message be
    missed, which this keeps; the cost is a spurious read now and then.

12. **Glob subscriptions are a subscription object.** `hg_sub_open("nav.*", …)`
    returns an `hg_sub` whose matches are opened as the directory word changes
    (`hg_glob_poll`, called by `hg_wait` and by a module on `hg_wait_fd` itself)
    and listed by `hg_glob_count`/`hg_glob_at`; reading the glob itself is
    `EINVAL`. `hg_wait` caps its sleep at 10 ms while a glob is in the set, as
    docs/01 says. The matches are allocated as they are found: the one allocation
    after open, and not on a hot path. Matches inherit the glob's start mode.

## Limits and sizes

The wiring is a static `struct hg_wiring` of about 90 KB (127 subs, 32 outputs
with 16 sources each, 32 serves, 32 calls, 64 mirrors). The registry of open
topics holds 512 subscriptions and 512 outputs per process. A subscription is
about 1.2 KB (two 512-byte paths). Field names are at most 63 bytes
(`hg_type_abi.md`), schemas at most 3584 bytes with 128 fields; the run-time
parser's `struct hg_schema` is about 12 KB and lives where the caller puts it.

## What stage 1 leaves out, and why

- The control socket, `page` and `open` requests, `PR_SET_DUMPABLE`: stage 2
  (they need the manager). `hg_init` reads the wiring and names the module;
  `hg_ready`, `hg_alive`, `hg_status` are no-ops.
- Calls (`hg_call_start`/`poll`, `hg_serve`, the envelopes), parameters, the
  electric-eye adapter, the Python helper: stage 2 and 3. The wiring parser
  already takes `serve`, `call` and `mirror` lines so a stage-2 wiring parses.
- `HG_BUSY` has no test: a reader overtaken eight times in a row needs a writer
  faster than any on the test machine for a 256-byte payload.
- The `strace -c`/seccomp count of wake system calls with no waiter is not a
  suite; the lost_wake run prints wake-ups against messages (one per publish with
  a sleeping reader, none otherwise), which the log shows.
- Running the suites on AArch64 and ARMv7 needs a full-system emulator or a
  board: qemu-user has no `futex_waitv`. The static aarch64 build links all eight;
  under `qemu-aarch64` the five that never sleep on a futex (layout, pubsub,
  wiring, prov, torn) run and pass, the other three fail at their first wait.
- Sanitizers: `check-tsan` and `check-asan` rebuild the hg suites with no
  suppression; a compiler without the runtime makes the target say `skip`. gcc,
  clang and `zig cc` 0.17 all pass both.

## Stage 2a additions (the manager's side: hangard/README.md)

- `hg_control.h/.c`: the control socket's wire format and client (`page`, `open`,
  the admin requests), one `SOCK_SEQPACKET` connection per request, a descriptor by
  `SCM_RIGHTS`; it retries while the manager is not listening, as docs/03 says a
  module waits for a restarting manager.
- `hg_init` sets `PR_SET_DUMPABLE` 0 first, always; with `HANGAR_CONTROL` set it then
  fetches the module page and maps it. `hg_ready`, `hg_alive`, `hg_status` write the
  page (no-ops without one), and a publish on the last unpublished rated output of the
  wiring sets `ready_instance` by itself (docs/02 "Ready is having published").
- `hg_pub_open` under a manager sends `open` with the hash, payload size and schema
  text and maps the descriptor it gets back (validated with the same header checks
  as a reader's); standalone mode is unchanged. A refusal maps to `EACCES`,
  `EPROTOTYPE` or `EBUSY` from the words of the manager's reason.
- `HG_SRC` in the Makefile gained `hg_control.c`; every stage-1 suite is unchanged.

## Stage 2b additions: calls and parameters on topics (docs/04, docs/05)

What `hg_call.h/.c` and `hg_params.h/.c` do, and where they depart from the
text. The manager's side (the implicit `serve set_params` line, the control
requests `call`, `method`, `param-read` and `param-write`, `hangar call` and
`hangar param`) is in `hangard/README.md`.

**Calls.** `hg_call_start` fills the request envelope (112 bytes, to the byte of
`msg/call_request_envelope.msg`; `struct hg_call_request_envelope` in `hg_call.h`
is the same layout with static asserts, so the library needs no generated file),
copies the request after it and publishes on `<module>.req` with `HG_NO_REFS`;
the `.req` output is opened on first use with the generated type `req:<module>`,
whose text the library builds exactly as the manager does (`hg_call_req_schema`:
the largest request among the wiring's `call` lines) and checks against the
wiring's hash. At most 15 calls are in flight (`HG_E_BUSY` for the sixteenth);
each has a reply buffer, allocated once at the first call (the one allocation,
15 times the largest reply among the calls). `hg_call_poll` reads the server's
`.rep` topic in order (opened `HG_OLDEST` at first use), keeps the replies whose
`caller_id` is this module's `.req` topic id and whose `call_id` is in flight,
copies them into their pending call's buffer and passes the rest over: another
caller's, a timed-out call's, or a reply received before the request was
published (a restarted caller counts from 1 again, and the ring may still hold
the predecessor's reply to its call 1). `hg_call` is start, then `hg_wait` on the
server's `.rep` topic until the reply or the timeout; refused in a module with an
actuator output.

`hg_serve` opens `<module>.rep` with the generated `rep:<module>` (largest reply
among the `serve` lines), reads every named `.req` subscription of the wiring and
the matches of `subglob *.req` from the oldest message still in each ring, and
before opening its `.rep` reads the predecessor's replies in the old file (through
`hg_sub_open_own`, new in `hg_topic.h`: a module may read its own output), so a
request already answered, whose reply's `received_ns` is at or after the request's
`publish_ns`, is skipped (docs/04 "While the server restarts"). The four steps of
docs/04 "Serving" run in the text's order; the reply carries a reference to the
request through `hg_publish_with` (the manager puts the request topics among the
`.rep` output's `Sources`; a hand-written wiring that does not gets unreferenced
replies and one log line). The topics are taken in turn, one request each round,
eight per pass; a message addressed to another server (the admin topic carries
everyone's) is passed over without counting, and nothing is read that the pass
cannot serve. A handler overrunning its deadline is logged on stderr.

**Parameters.** The attributes (default, range, `@restart`) are parsed from the
type's own schema text, which the generated descriptor carries verbatim
(`hg_params_attrs`); the generated `*_META` array is the same information for
code that wants it as C data. Loading is docs/05's: no file is the first start
(defaults written, version 1, `changed_by` "saved"); a file without the header
line, an empty one, an unknown or repeated field, a value that does not parse or
is out of range, or a validator refusal end the process with status 78 after the
reason is put on the status line; a missing field takes its default and is
logged. `set_params` is registered through the ordinary `hg_serve_method`, so the
Allow check, the type check and the reference to the request are the call
library's; the handler does steps 2 to 6 of docs/05 "Setting parameters" and,
for a `@restart` change, saves, publishes `restart_pending` with the values in
force, answers `restarting = 1`, and asks for `restart-self` once the reply is out
(`hg_serve_after`, a per-method hook for the library's own use), repeating the
request while the manager is not listening. The file is written to a temporary
name and renamed, with no fsync.

### Design deviations, stage 2b

13. **The wiring carries three things docs/03's table does not.** A `role` line
    (the module's own role: a server names a mirrored caller `<role>:<module>`
    with the other side's, and docs/04 says the wiring carries it), the bare word
    `actuator` on a `pub` line (docs/04: `hg_call` is refused in a module with an
    actuator output, which the library could not know otherwise), and
    `noroute=1` on a `call` line nothing serves on either side (docs/04:
    `HG_E_NOROUTE` "known from the wiring"; without the flag the only sign was a
    `sub <server>.rep` line without a type). The manager writes all three;
    `hg_wiring_parse` accepts them and a wiring without them still parses.

14. **A validator has no ctx argument.** docs/05's example is
    `validate(candidate, current, reason)` with `hg_params_open(..., validate,
    NULL)`; the signature is kept to the letter, and the pointer is reachable as
    `hg_params_ctx()`. Handlers get theirs as `c->ctx`, so docs/04's
    `on_arm(c, req, rep, reason)` is also unchanged.

15. **`hg_call_start` returns a status code**, `HG_OK` or `HG_E_BUSY`,
    `HG_E_NOROUTE`, `HG_E_BADTYPE`, and `HG_E_NOMETHOD` with errno `EACCES` for a
    call the module did not declare (docs/04: "the library refuses a call its
    module did not declare", without saying how). docs/04 shows the call without
    a result. A timeout of 0 or less is the default 100 ms.

16. **The reply type of a refused request is the method's.** docs/04 leaves
    `reply_type` unspecified when the body is absent; it is the method's reply
    hash when the method exists, 0 for `HG_E_NOMETHOD`, so a reader can decode the
    envelope's intent. `received_ns` is set on every reply, expired ones included.

17. **A reply from before the request is not its answer.** Call ids count from 1
    per caller instance (docs/04) and the `.rep` ring may still hold the
    predecessor's replies, so `hg_call_poll` also requires `received_ns` at or
    after the request's publish time. The same rule, the other way, lets a
    restarted server tell an answered request from a restarted caller's call with
    the same id.

18. **A `@restart` save is marked in the file's second line** (`, restart` after
    the time): the new instance publishes with the requester's name and rewrites
    the line without the mark, so a later plain start says `saved` as docs/05
    wants both. Nothing in docs/05 says how the new instance tells the two
    starts apart.

19. **The `.params` topic's header carries the parameter type's text verbatim**,
    attributes included (the manager's generated `params:<module>` text has the
    canonical lines only; both hash the same, since the hash ignores attributes),
    so a UI builds its controls from the header as docs/05 suggests. The
    canonical lines are used only when the verbatim text would not fit 3584 bytes.

20. **The saved file lives under `$HANGAR_STATE_DIR` when set**, else
    `$HANGAR_ROOT/var/lib/hangar/<module>/params` as docs/05 says. The manager
    sets neither (the module's state directory is its working directory, which
    the library does not rely on); `hangar run` sets the first so a module run by
    hand has a writable file beside its private topics.

21. **Replies left behind by a restart are only as old as the ring.** The manager
    creates a fresh file at each restart (`write_seq` continued, slots empty), so
    a restarted server sees only its predecessor's replies, not the one before:
    a request answered two restarts ago, still in time, is served again. docs/04
    allows exactly this ("whose reply has since left the ring, and which is still
    in time, is served again") and asks for idempotent methods; the suites had to
    wait out request deadlines before restarting a module twice.

22. **`hg_serve_method` refuses a method the wiring does not list** (`EACCES`), as
    `hg_pub_open` refuses an undeclared topic; a served method with no handler
    registered answers `HG_E_NOMETHOD` ("no handler registered"), distinct in its
    reason from a method the wiring does not know.

### What stage 2b leaves to the bridge and later

- `hg_pub_open_mirror` writes this module's name as the header's `writer`;
  docs/01 "Mirrored topics" wants the sending module's name there, which the
  bridge knows from the handshake. The bridge needs a writer argument on that
  open (and the manager's `open` a `writer=` key), or the caller token of a
  mirrored request is `<role>:bridge`. The token code itself is ready: the
  `hg_call` suite creates the mirrored topics as the sending module.
- A server's `subglob *.req` is opened and polled by `hg_serve`, its matches
  served and deduplicated against the named subscriptions; the manager does not
  yet write the line (no bridge configuration), nor `*.req` among the `.rep`
  output's `Sources`, so a glob match's reply would go out unreferenced.
- The bridge rewrites `deadline_ns` and `received_ns` into the receiver's clock;
  nothing in the library assumes otherwise, but nothing tests it.
- Dataset row types and the policy types (docs/06, docs/09) are untouched: a
  `Type=dataset:<name>` output is still refused by the manager.

## Phase 1 additions (modules/PHASE1.md): the gate, the actuator line, the shim

- `hg_gate.h/.c`: the actuator gate of docs/07 "The actuator gate". `hg_gate_open`
  takes the Command topic and the two timeouts from the wiring's `actuator` line and
  opens the two subscriptions itself (a topic read through two subscriptions of one
  process would make its provenance ambiguous, so the module reads `safety.state` and
  the command through the gate: `hg_gate_subs`, `hg_gate_safety`,
  `hg_gate_command_info`). `hg_gate_check` returns the newest command only when
  `safety.state` is fresh and says `outputs_enabled`, the command is within
  `CommandTimeout`, and it is not flagged as computed from stale inputs; the reasons are
  docs/07's, with a closed or dead writer named as such before staleness. The stale
  input is named by topic id ("computed from stale inputs (topic 79cc07e9)"), not by
  name: the name is in the publisher's wiring, not the actuator's (docs/07's example
  says "nav.depth"). Suite `test_hg_gate`.
- **Deviation 23: an `actuator` wiring line.** docs/03's wiring table has no place for
  `[Actuator]`'s Command and timeouts, which docs/07 says the gate takes "from the
  wiring". The manager writes `actuator command=<topic> command_timeout=<d>
  safety_timeout=<d>` after the `role` line; `hg_wiring_parse` fills
  `hg_wiring.actuator`; a wiring without the line parses as before.
- **Deviation 24, `until the bridge`: the same-host shim.** With `$HANGAR_PEER_TOPIC_DIR`
  set (and, under a manager, `HANGAR_PEER_SHIM=1`, which the manager sets when it was
  itself started with the variable; refused with a line on stderr otherwise), a
  subscription with no file in the module's own topic directory reads the other root's
  file read-only, and `hg_sub_open("<role>:<topic>")` reads the peer's file outright;
  `hg_glob_poll` scans both directories (a server's `subglob *.req` finds the other
  root's request topics), `hg_wait` also sleeps on the peer directory's word, and
  `hg_sub_is_peer` marks such a subscription, which `hg_call_caller_token` counts as
  mirrored: a request read from it bears the other side's role
  (`topside:topside_mavlink`). The manager, started with the variable, writes `remote=1`
  instead of `noroute=1` on a `[Call]` to a method nothing here serves, adds `subglob
  *.req` (and `*.req` among the outputs' `Sources`) to a server whose `Allow` names the
  other role, and passes the two variables to its modules. Everything here is marked
  `until the bridge` and goes when the bridge writes mirrored topics into the local
  directory; nothing in the modules knows which way a topic arrived.
