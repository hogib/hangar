# hangar spike, C11

`./build.sh` builds `spike-manager`, `talker`, `listener` and `bench`, static musl via
`zig cc` 0.17, stripped (`llvm-strip --strip-all`), into `out/<target>/` (`-Os`) and
`out/<target>/O2/` (`-O2`), for `x86_64-linux-musl` and `aarch64-linux-musl`, and writes
`out/report.json` (sizes, line counts, build time). Flags: `-std=c11 -Wall -Wextra
-pedantic -Werror -static`; also clean under gcc 16 and clang with the same flags.
`./check.sh` runs the short correctness checks below.

Layout: `src/hg_topic.[ch]` + `src/hg_sys.h` (library), `src/manager.c`,
`src/talker.c`, `src/listener.c`, `src/bench.c`.

## Numbers (from `out/report.json`)

| binary | x86_64 -Os | x86_64 -O2 | aarch64 -Os | aarch64 -O2 |
|---|---|---|---|---|
| spike-manager | 50 504 | 55 064 | 56 984 | 62 768 |
| talker | 48 160 | 50 504 | 55 016 | 59 936 |
| listener | 53 912 | 62 760 | 59 008 | 66 368 |
| bench | 49 128 | 59 200 | 54 544 | 61 528 |

Lines (non-blank, non-comment): library 920, manager 450, programs 562.

Build time from clean, x86_64 release (4 binaries, compiled in parallel): 0.22 s. All
16 binaries: under 1 s. "Clean" means a fresh object cache (`ZIG_LOCAL_CACHE_DIR` in
`out/`, deleted each run); musl itself stays prebuilt in zig's global cache, as Rust's
std does. A cold global cache adds about 11 s once per target (zig compiles musl).

## Correctness checks (x86_64, short runs, not timing)

- `bench torn`, 3 s, 4 slots, 4 reader processes alternating `latest()`/`next()`:
  19.6 M published, 77.6 M read, **torn 0**, busy 0. Sanity check of the test itself:
  with the seqlock's second `seq` load removed, a 2 s run reports 5 984 torn.
- `bench lost_wake`, 20 000 messages at random 0-200 µs gaps: 20 000 received,
  **gaps 0, timeouts 0**. Also 50 further runs of 3 000 messages (-Os and -O2): all clean.
- Manager, talker at 1 kHz with `--crash-after 500` plus a listener, 4.5 s: five
  aborts, restarts after 0.1, 0.2, 0.4, 0.8 s (next 1.6 s), registry correct (pid
  and start time, `backoff`, failures, delay). Listener: `msgs 2500, gaps 0, torn 0,
  reopens 4`, p50 5.8 µs. On SIGTERM the listener prints its line and exits 0, the manager exits
  0 and removes the registry.
- Also checked: an exec failure is logged as `cannot run <program>: <error>` and
  backs off; a module ignoring SIGTERM is SIGKILLed after 500 ms, its group reaped; the
  manager made 0 context switches over 3 s with modules running and nothing to
  restart.

## Bugs found, and deviations from SPEC.md

1. **Lost wake-up in the publish order as specified** (spec §1 publish step 4 and
   `docs/01-topics.md` "Publishing"). With `fetch_add(futex_word)` *then*
   `exchange(wait_flag, 0)`, this interleaving loses a wake-up: the writer stores
   `write_seq = n`; a reader (still in a previous `wait()`) returns and reads n; the
   writer increments `futex_word`; the reader's next `wait()` reads `futex_word`, sets
   the flag, finds nothing new; the writer's exchange now takes that fresh flag and
   calls `FUTEX_WAKE` on nobody (the reader is not queued yet); the reader sleeps on
   the current value, and every later publish finds the flag clear. The reader sleeps
   until its timeout. Seen in about 10-20 % of `lost_wake` runs (the reader stalled
   right after message 1); instrumented to confirm (publish 1: flag 1, woke 0;
   publish 2 onwards: flag 0). **Fix:** exchange the flag *before* incrementing
   `futex_word`, and `FUTEX_WAKE` after the increment if the flag was set. A publish
   that takes a reader's flag then changes `futex_word` after that reader read it, so
   the reader's `futex_waitv` either fails with `EAGAIN` or is queued in time to be
   woken. 0 failures in 100+ runs since. The design doc should change its step 4.
2. **`wait()` returned "ready" after a kernel timeout** if news had arrived in the
   meantime, so `lost_wake` could never count a lost wake-up (it printed `timeouts 0`
   in the failing runs above). `hg_wait` now returns 0 (timeout) whenever
   `futex_waitv` timed out at the caller's deadline; callers read afterwards anyway.
3. **`wait()` and signals.** It first swallowed `EINTR`, so a listener ignored
   SIGTERM until the manager killed it; returning -1/`EINTR` was not enough either:
   a handler that runs while `hg_wait` is in user space (its poll loops, `attach()`)
   or between the caller's check of its flag and the sleep goes unseen until news
   or the deadline. Now `hg_wait_interrupt()` (async-signal-safe) sets a
   process-private futex word that every `hg_wait` checks at the top of each pass
   and also sleeps on (one of the two slots the design reserves next to the 126
   topics), so the interrupt makes the sleep fail or wakes it. The listener's
   handler calls it.
4. **`write_seq` is stored seq_cst, not release** (spec §1 publish step 3). The
   writer's `write_seq` store then flag exchange, against the reader's flag store
   then `write_seq` load, is a store-buffering pattern: with a release store, C11
   allows both sides to miss each other (a lost wake-up), and a compiler may move
   the store after the exchange. All four are now seq_cst (the reader's loads
   already were: an acquire load may be done before the preceding store, `LDAPR` on
   ARMv8.3+). Cost: AArch64 emits the same `STLR`; x86 an `XCHG` instead of a `MOV`,
   within noise (publish16 26.5 vs 26.4 ns, short runs). `docs/01-topics.md` step 3
   should say seq_cst.
5. **`wait()` re-check of flags**: the spec says to return ready when flags bit 0/1/2
   is set. Taken literally, a reader of a dead topic spins at 100 % CPU until the
   restart. Implemented as "flags differ from what `latest()`/`next()` last
   reported", so a reader sleeps after it has seen `HG_CLOSED` and wakes when bit 2
   arrives.
6. **Polling where the spike has no futex**: an `ABSENT` topic is retried every 10 ms
   inside `wait()` (the design's directory word is not in the spike). After bit 2, if
   the new file is not yet renamed into place (normally a window of microseconds),
   `wait()` retries the path after 1 ms, backing off to once a second. A replacing
   writer that dies between flagging the old file and its rename would otherwise
   leave every reader polling at 1 kHz; after a second without a new file,
   `latest()`/`next()` report `HG_CLOSED`, and any later announcement on the old
   file (the next writer flags it again) restarts the 1 ms retries. `hg_pub_open`
   clears bit 2 again, with a wake, if its rename fails.
7. **Starting point of a new reader**: `next()` on a first attach starts at the oldest
   message still safely in the ring (`W−K+2`), not at `first_seq + 1`, so joining a
   long-running topic does not report the messages before the reader existed as gaps.
8. **listener** records latency only for messages published after it started; the
   backlog found in the ring at attach is read and checked but is not a wake-up.
9. **talker** has an extra `--slots` option (default 16), and calls
   `prctl(PR_SET_DUMPABLE, 0)` as `hg_init` would, which also keeps `abort()` from
   writing core dumps during the restart test.
10. **Manager**: a `pipe2` or `clone3` failure is logged and backed off like an exit,
    not fatal. It resets `SIGCHLD` to `SIG_DFL` before spawning (an inherited
    `SIG_IGN` had the kernel reap every module: no exit status, and a group kill
    after the group id was free). The exec-error pipe is read before the registry
    is written, so a program that cannot run is never shown `running`; that child
    is reaped at once through its pidfd and backed off. The config is refused for a
    line of more than 255 words, a topic name the library would reject (`../x`),
    or a topic written by two modules. There is no fork fallback. State names are `running`, `backoff`,
    `stopping` and `stopped`. The child sets `CLOSE_RANGE_CLOEXEC` on fds above 2
    instead of closing them, so the error pipe stays open until `execve`, and every
    other descriptor still closes at exec. The registry is removed on a clean stop,
    as `docs/03-manager.md` "Shutdown" says. `bench` options (`--runs`,
    `--count`, `--torn-seconds`, `--lost-msgs`, test names) select subsets; a
    reader that exits before it is ready fails the run instead of hanging it.
11. **Library API beyond the spec**: `hg_publish` takes the payload length and
    refuses (-1/`EINVAL`) one that is not the topic's `payload_bytes`, like the read
    side's buffer check. `HG_ABSENT` means only that no file is at the path; a file
    that cannot be opened or mapped, or is not a compatible topic (`EPROTO`: other
    version, bad sizes), gives `HG_ERROR` with `errno`, and `wait()` returns at once
    for it instead of polling. The reader copies the header once (atomic word
    loads), validates the copy, and uses only that, so a writer that changes
    `slot_count` or `slot_stride` after the check cannot send a reader outside its
    mapping. `listener` and `talker` range-check their numbers before converting
    them (UBSan clean at the limits).

## What was awkward in C

- The seqlock payload is copied as relaxed atomic 8-byte words on both sides (the
  payload is 8-aligned and its slot room rounds up to 8), so no access races in C11
  terms; a plain `memcpy` would be a data race the seqlock check only discards
  after the fact. It is also faster here than musl's `memcpy` for 256 bytes
  (publish 29 vs 117 ns, latest 19 vs 111 ns, short runs).
- No libc wrapper or headers for `futex_waitv`, `clone3`, `close_range`, `P_PIDFD`:
  their structs and numbers are declared by hand in `hg_sys.h` (the `<linux/*>`
  headers clash with musl's). The new syscall numbers are the same on x86_64 and
  aarch64.
- `clone3` through `syscall()` returns twice like `fork`, but bypasses libc's fork
  handling (musl's cached tid is stale in the child). It works because the child only
  calls async-signal-safe functions before `execve`.
- Layout correctness rests on `_Static_assert(offsetof(...))` for every field, which
  C makes easy.
- The memory-ordering subtleties (items 1 and 4) are language-independent; C neither
  helped nor hindered beyond making every access's ordering explicit. Item 4 was
  first left as release because the hardware keeps it in order; only the language
  model does not.
- aarch64 binaries are built only, never run (QEMU user mode lacks `futex_waitv` and
  `clone3`).
