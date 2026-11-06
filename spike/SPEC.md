# Language spike: C11 vs Rust

The same thin slice of hangar, built twice, from this spec, by two independent
implementations, then measured and reviewed side by side. It decides the language;
it is not the product. Design references: `docs/01-topics.md` (topics) and
`docs/03-manager.md` (manager).

Each implementation lives in its own directory (`spike/c/`, `spike/rust/`) and must
not look at the other.

## Rules for both

- **No dependencies** beyond the language's standard library and the C library: no
  crates (not even `libc`; declare the few C functions needed with `extern "C"`), no
  third-party C libraries. System calls without a libc wrapper (`futex_waitv`,
  `clone3`, `pidfd_open`, `pidfd_send_signal`, `waitid` with `P_PIDFD`) go through
  `syscall(2)`.
- **Static musl release builds**, stripped, for `x86_64` and `aarch64`:
  - C: `zig cc -target x86_64-linux-musl` / `aarch64-linux-musl`, `-std=c11 -Os`,
    `-static`, then strip. Also report `-O2` sizes.
  - Rust: targets `x86_64-unknown-linux-musl` and `aarch64-unknown-linux-musl`
    (installed), release profile `opt-level = "s"`, `lto = true`, `codegen-units = 1`,
    `panic = "abort"`, `strip = true`. Also report `opt-level = 3` sizes. A linker for
    aarch64 may be a `zig cc -target aarch64-linux-musl` wrapper script.
- One build script per implementation (`build.sh`) that produces every binary for both
  targets into `out/<target>/`.
- Idiomatic code for the language. Rust: `unsafe` only where it is unavoidable
  (shared memory, system calls), each block with a `// SAFETY:` comment; safe wrappers
  above it. C: C11 `<stdatomic.h>`, `-Wall -Wextra -pedantic` clean.

## 1. The topic library

A subset of `docs/01-topics.md`, exactly as specified there where it is covered here.

**Files.** In `$HANGAR_TOPIC_DIR` (required). Topic file `<dir>/<name>`; wake file
`<dir>/.<name>.wake` (64 bytes, mode 0660). Native byte order (little-endian).

**Header** (4096 bytes; fields not listed are 0):

| Offset | Type | Field |
|---|---|---|
| 0 | char[8] | `magic` = `"HGTOPIC1"` |
| 8 | u32 | `version` = 1 |
| 12 | u32 | `header_bytes` = 4096 |
| 16 | u64 | `generation` (random per writer run) |
| 24 | u32 | `slot_count` (power of two) |
| 28 | u32 | `slot_header_bytes` = 128 |
| 32 | u32 | `payload_bytes` |
| 36 | u32 | `flags`: bit 0 closed, bit 1 writer died, bit 2 replaced (atomic) |
| 40 | u64 | `slot_stride` = round_up(128 + payload_bytes rounded up to 8, 64) |
| 48 | u64 | `slots_offset` = 4096 |
| 56 | u32 | `topic_id` = FNV-1a 32 of the name |
| 72 | char[48] | `name` |
| 168 | u64 | `first_seq` |
| 192 | u32 | `futex_word` (atomic) |
| 200 | u64 | `write_seq` (atomic) |
| 208 | u64 | `last_publish_ns` (atomic) |

**Slot header** (128 bytes): 0 `seq` u64 (atomic; `2n` complete, odd while written),
8 `stamp_ns` u64, 16 `publish_ns` u64, 24 `n_inputs` u32, 28 `flags` u32, 64 `inputs`
`{u32 topic_id, u32 flags, u64 seq}[4]`. Payload follows at offset 128.

**Writer**
- `open(name, payload_bytes, slot_count)`: if a file of that name exists and is a
  valid topic, read its `write_seq` as W (else W = 0). Create `<dir>/.<name>.tmp.<pid>`,
  `posix_fallocate` it, write the header with `write_seq = first_seq = W`, create the
  wake file if missing, then: if the old file exists, set its flags bit 2, increment
  its `futex_word` (seq_cst) and `FUTEX_WAKE` all on it; then `rename` the new file
  into place. Map it read-write `MAP_SHARED`.
- `publish(payload, stamp_ns)` for message n = write_seq + 1 into slot (n−1) mod K:
  1. store `seq = 2n−1` (relaxed), release fence;
  2. write `stamp_ns`, `publish_ns` (CLOCK_MONOTONIC), `n_inputs = 0`, the payload;
  3. store-release `seq = 2n`; store-release `write_seq = n`; store (relaxed)
     `last_publish_ns`;
  4. `fetch_add(futex_word, 1)` (seq_cst); `exchange(wait_flag, 0)` (seq_cst) on the
     wake file; only if the old value was non-zero, `FUTEX_WAKE` (shared, not private)
     all on `futex_word`.
- `close()`: set flags bit 0, increment `futex_word`, `FUTEX_WAKE` all.

**Reader**
- `open(name)`: open read-only, map `PROT_READ` `MAP_SHARED`, validate magic, sizes and
  file size; open the wake file read-write and map it. If absent, return an `ABSENT`
  handle that attaches on a later call.
- `latest(&buf, &info)`: n = load-acquire `write_seq`; if n == last returned or
  n <= `first_seq` → `NONE`; slot (n−1) mod K; s1 = load-acquire `seq`; if s1 != 2n,
  retry (at most 8 times, then `BUSY`); copy header fields and payload; acquire fence;
  s2 = load `seq`; if s2 != s1 retry. On success `MSG`.
- `next(&buf, &info)`: in order, per `docs/01-topics.md` "In order": want w = last+1
  (at least `first_seq + 1`); with W = write_seq, K = slot_count: w > W → `NONE`;
  w <= W−K+1 → count the skipped messages in `info.gap` and set w = W−K+2; otherwise
  read slot (w−1) mod K: `seq == 2w` → copy with the before/after check; odd or > 2w →
  message w lost: count in gap, w+1, never retry the same target.
- `wait(readers[], n, timeout_ms)`: for each reader read `futex_word` (acquire) into
  v[i]; for each, store `wait_flag = 1` (seq_cst); re-check each reader: if
  `write_seq` is beyond what it last returned, or `flags` has bit 0/1/2 set, return
  ready without sleeping; else `futex_waitv` on all `{futex_word, v[i]}` (32-bit,
  shared) with an absolute `CLOCK_MONOTONIC` timeout. Returns which readers may have
  news (or timeout).
- **Replacement**: when flags bit 2 (or bit 1, after a new file appears) is set, the
  reader drains what it wants from the old mapping, then reopens the path, and
  continues `next()` across the files by sequence number (`first_seq` of the new file
  equals the old file's last `write_seq`).

## 2. The mini-manager

`spike-manager <config>`; config is text, one module per line:
`<name> <topic-it-writes-or-'-'> <program> [args...]`.

- One thread, one `epoll`, on: a `signalfd` (`SIGTERM`, `SIGINT`, `SIGCHLD` blocked
  and read from it), one pidfd per module, one `timerfd` for restart back-offs.
- Spawn with `clone3` + `CLONE_PIDFD` (falls back to nothing: required). In the child:
  `setpgid(0,0)`, reset all signal dispositions to default and unblock all signals,
  close every descriptor above 2 (`close_range`), exec. An exec failure is reported to
  the parent over a `O_CLOEXEC` pipe; the parent logs "cannot run <program>: <error>".
- Registry: after each state change, write `$HANGAR_TOPIC_DIR/../registry`
  atomically (temp file + rename): `hangar-registry 1` then per module
  `name pid starttime state failures delay_s`, start time read from `/proc/<pid>/stat`
  field 22.
- On a pidfd becoming readable: `waitid(P_PIDFD, fd, &info, WEXITED | WNOWAIT)`; if
  the module writes a topic, open that topic's header read-write, set flags bit 1
  (fetch_or, release), `fetch_add` its `futex_word` (seq_cst), `FUTEX_WAKE` all; then
  `kill(-pgid, SIGKILL)` (ESRCH ignored); then reap (`waitid` without `WNOWAIT`); log
  the exit status; schedule a restart after the back-off (100 ms, doubling, at most
  2 s; reset after 5 s running).
- On `SIGTERM`/`SIGINT`: `SIGTERM` every module's group, wait up to 500 ms (still in
  the event loop), `SIGKILL` the rest, reap, exit 0.
- Idle: with nothing to restart, the manager must not wake at all (no periodic timer).

## 3. Programs for the measurements

- `talker <topic> <rate_hz> [--payload 256] [--crash-after N]`: publishes at the rate
  (absolute `clock_nanosleep`), with the payload filled with the byte `(seq & 0xff)`;
  with `--crash-after`, calls `abort()` after N messages.
- `listener <topic> <seconds>`: waits with `wait()`, reads with `next()`, and for each
  message records the latency `now − publish_ns` at wake-up; checks every payload byte
  equals `seq & 0xff`; at the end prints one line of JSON: `{"msgs":…, "gaps":…,
  "torn":…, "p50_us":…, "p99_us":…, "max_us":…, "reopens":…}`.
- `bench`, printing JSON, each figure the median of 5 runs:
  1. `publish_ns`: ns per publish of 256 bytes, no reader waiting, 5 000 000 publishes;
  2. `publish16_ns`: the same with 16 bytes;
  3. `latest_ns`: ns per `latest()` of 256 bytes against a writer idle between calls,
     5 000 000 calls (alternate publish/latest so each call returns a message; report
     the latest part only, measured as total minus a publish-only loop);
  4. `torn`: one writer process publishing patterned 256-byte messages as fast as it
     can into a 4-slot ring for 10 s, four reader processes doing `latest()` and
     `next()` and checking every byte; report messages read and torn (must be 0);
  5. `lost_wake`: a writer publishing 100 000 messages at random gaps of 0–200 µs, a
     reader in `wait(timeout = 10 s)` + `next()`; report messages received, gaps, and
     the number of waits that ended by timeout (must be 0).

## 4. What to report (`out/report.json` plus a short `NOTES.md`)

- Binary sizes (stripped, static, musl) for `spike-manager`, `talker`, `listener`,
  both targets, both optimization levels.
- Lines of code (non-blank, non-comment) of the library and the manager; for Rust, the
  number of `unsafe` blocks and lines inside them.
- Build time from clean (release, x86_64).
- Anything in the spec that was hard, ambiguous or impossible in the language, and how
  it was resolved.

The measurement itself (latency under the manager, restart behaviour, RSS, idle
wake-ups) is run afterwards on both implementations by one person on one machine.
