# Rust implementation: notes

`./build.sh` builds from clean: `out/<target>/` (opt-level "s") and `out/<target>/o3/`
(opt-level 3) for x86_64 and aarch64 musl, then writes `out/report.json` (sizes, lines
of code, `unsafe` counts, clean build time). The aarch64 target links with the
`rust-lld` that rustup ships (`.cargo/config.toml`); no zig wrapper was needed.

Layout: `hangar/` (library: `sys.rs` system interface, `topic.rs` topics),
`manager/` (`spike-manager`), `programs/` (`talker`, `listener`, `bench`).

## Checks run (x86_64, release musl)

- `bench torn --seconds 3 --runs 1`: 22.2 M published, 67.1 M reads, 0 torn.
- `bench lost_wake --messages 20000 --runs 1`: 20000 received, 0 gaps, 0 timeouts.
- Manager with `talker --crash-after 500` at 1 kHz plus a 6 s listener started once
  the topic exists: talker restarted with back-off 0.1, 0.2, 0.4, 0.8, 1.6 s; listener
  `gaps 0, torn 0, reopens 5`; an unrunnable program logs `cannot run ...`; SIGTERM
  stops all, exit 0. A listener started before the talker has created the topic
  attaches on the 100 ms absent poll and reads from the file's start: the ~85
  messages the 16-slot ring no longer holds count as gap (the spec's directory word
  would remove this; the first version behaves the same).
- Idle manager: no context switches over 3 s. Modules ignoring SIGTERM are SIGKILLed
  after 500 ms; a module whose leader left its process group is still stopped (pidfd);
  a closed stderr pipe does not stop the manager.

## Choices where the spec is open

- **Seqlock without data races.** Every slot header field and every payload word is
  an atomic; payloads are copied as 64-bit relaxed loads/stores (the stride makes the
  rounded-up words fit). The reader's fence is `fence(Acquire)` before the second
  `seq` load, the writer's is `fence(Release)` after the odd store.
- **Read-only mappings.** Rust allows only `Relaxed` atomic loads (of at most 8 bytes)
  on memory mapped `PROT_READ`. The reader's ring is a `Ring<ReadOnly>`, whose
  accessors make relaxed loads followed by `fence(Acquire)` (and `wait` puts a
  `fence(SeqCst)` between its `wait_flag` stores and the re-checks); only a
  `Ring<ReadWrite>` hands out the header for stores and read-modify-writes.
- **`wait()` and ended topics.** "Return ready if flags has bit 0/1/2 set" would spin
  while a dead writer waits for its restart. The reader remembers which end bits it has
  already reported, and whether it acted on bit 2; `wait` returns for bits not yet
  acted on. The new writer's bit 2 on the old file then wakes it.
- **Draining before leaving a file.** After seeing an end or replace bit the reader
  loads `write_seq` again (the bit is set after the writer's last store), so a message
  published just before a close or crash is read before `Closed` or the reopen.
- **Bit 2 after rename.** The spec flags the old file before the rename; this flags it
  just after, so a reader that sees bit 2 always finds the new file at the path and
  never has to retry. If the reader cannot open the file it finds there (permissions,
  another format version, out of descriptors), reads return `Status::Error`
  (`last_error()` says why) and `wait` looks at the path every 100 ms instead of
  reporting the bit again. A closed or dead topic is also left for a different file
  found at the path; a new file whose sequence starts below the reader's (its writer
  did not find the old file) is read from its start.
- **Payload growth.** A replacing writer may use a larger payload. A read with a
  buffer shorter than `payload_bytes()` returns `Status::TooSmall` without consuming
  anything; the caller grows the buffer and calls again (the listener does).
- **Errors are not absence.** Only a missing topic is `Absent`. `Subscriber::open`
  returns other attach errors; later attach failures read as `Status::Error`. A writer
  whose existing topic it cannot open read-write fails rather than restart the
  sequence at 0 under that topic's readers.
- **Temporary file.** `.<name>.tmp.<pid>.<random>`, created with `O_EXCL`, so two
  opens of one topic in one process never share (and truncate) each other's file.
- **Absent topics.** There is no directory word in the spec, so `wait` retries the
  attach every 100 ms while a subscription is absent (or its replacing file cannot be
  opened); this is the only polling.
- **Where reading starts.** A subscriber that finds the topic at `open` starts with the
  next message published; one that attaches later starts at the file's `first_seq`.
- **`info.gap`** is set by every `next()` call, whatever it returns; callers sum it.
- **Registry** states are `running`, `backoff`, `stopping`, `stopped`; it is removed
  when the manager exits, as in `03-manager.md`. Back-off resets when a module ran 5 s,
  checked when it exits (no timer, so an idle manager never wakes).
- **Stopping.** SIGTERM and, at the 500 ms deadline, SIGKILL go to the process group
  and, with `pidfd_send_signal`, to the leader (which may have left its group). The
  manager never blocks on a process that has not exited: `waitid` uses `WNOHANG`, and
  exits after SIGKILL arrive through the pidfds in the event loop, so a module stuck in
  uninterruptible sleep delays only the exit, not signal handling.
- **Failures of the manager itself.** Log lines ignore write errors (Rust ignores
  SIGPIPE, so a closed stderr pipe gives EPIPE, and `eprintln!` would abort). A fatal
  error in the event loop, or a module whose pidfd cannot be watched, SIGKILLs every
  module and flags its topic before the manager exits.
- **Program resolution** happens in the parent (`$PATH` search) before `clone3`;
  the child setpgid's itself and the parent does too, so the group exists before any
  signal. The config is whitespace-split, no quoting.
- **bench**: torn and timeouts are summed over the runs (must be 0); other figures are
  medians. Options `--runs --count --seconds --messages` and test names select subsets.

## What was awkward in Rust

- No `libc` crate: every struct (`sigset_t`, `siginfo_t`, `clone_args`,
  `futex_waitv`, the packed x86_64 `epoll_event`) and constant is hand-written with
  size asserts; `futex` and `waitid` numbers differ per architecture.
- `clone3` in a Rust program is fork-like; the child must avoid allocating and taking
  locks (argv/envp are built before the call). It resets signals with raw
  `rt_sigaction`/`rt_sigprocmask` system calls, since musl's `sigaction` takes a lock
  for SIGABRT, so `spawn` is safe from a multi-threaded process too.
- Binary size is dominated by std (formatting, panic and backtrace machinery remain
  even with `panic = "abort"`); shrinking it needs nightly `build-std`, not allowed.
- Rust edition 2024's `unsafe extern` with `safe fn` items lets trivially safe C
  calls (`kill`, `setpgid`, `timerfd_create`) skip an `unsafe` block.
