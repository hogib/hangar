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
| `hg_topic.h/.c` | the topic file, writer, reader, provenance, `hg_wait`, `hg_wait_fd`, `hg_init` and the module page stubs |
| `hg_manager_stub.h/.c` | test-only: what the manager does to topic files, plus the publish protocol taken apart for the order guard |
| `tests/` | eight suites, one per area, on `lib/common/test_harness.h` |

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
