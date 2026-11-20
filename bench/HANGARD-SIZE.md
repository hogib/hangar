# hangard's size

docs/00 sets the manager's target at **under 100 KB** stripped and static, twice the
50 KB of the language spike's manager (topics, spawning, pidfds, the registry and the
control socket). The release's `hangard` is **292,784 bytes** (aarch64, musl, zig cc 0.17,
`-O3`, `-static -s`; 2026-11-20, main at `bfe3bfb`). This is where the bytes are, and what
would bring them down. Measured only: nothing in `hangard/` was changed.

**Since 2026-11-20 the release is built with the flags measured below** (`tools/release.sh`:
`hangard` and `hangar` only, in a build directory of their own; the modules keep `-O3`).
The manager grew in the meantime, so the result is **201,040 bytes, 1,040 over the
restated target of 200 KB**: see [Applied](#applied-2026-11-20) at the end. The rest of
this file is the measurement as made at `bfe3bfb`.

## Method

The release's build (`make BUILD=release CC="zig cc -target aarch64-linux-musl
-mcpu=cortex_a72" LDFLAGS="-static -s"`), once stripped and once not; `readelf -S` for the
sections; `nm -S` for the symbols, each address counted once (zig's runtime gives many
functions two names, `qsort` and `c.stdlib.qsort`), attributed to hangard, `lib/hg` or
`msg/` by the object files that define them, and to libc and the runtime otherwise.
zig cc does not take `-Wl,-Map`, so there is no linker map.

## Where the bytes are

| Section | Bytes |
|---|---|
| `.text` | 227,052 |
| `.rodata` | 30,082 |
| `.eh_frame` + `.eh_frame_hdr` | 32,736 |
| `.data` | 1,360 |
| `.bss` (no file bytes) | 1,397,664 |

| Origin (file-backed symbols) | Bytes | |
|---|---|---|
| libc and zig's runtime | 88,336 | 38 % |
| hangard | 83,104 | 36 % — largest: `mf_set_check` 11.4 KB, `mf_parse_text` 11.0 KB, `main` 8.1 KB, `control_handle` 4.4 KB, `module_spawn` 3.3 KB |
| `lib/hg` | 60,738 | 26 % — `hg_wiring_parse` 3.9 KB, `hg_params_open`, `pub_open`, `hg_wait_ready`, `hg_schema_parse`, `hg_serve`, 2–2.4 KB each |
| `msg/` | 672 | |

libc and the runtime by family: number parsing and scanf (`__floatscan`, `vfscanf`,
`hexfloat`, `__intscan`) 13.5 KB; sorting (`qsort` as zig's pattern-defeating quicksort)
11.0 KB; `printf_core` and its helpers 8.5 KB; time zones and `strftime` 6.5 KB; the
128-bit float runtime (`__divtf3`, `__multf3`, `add_f128`, pulled in by `long double` in
scanf and printf) 5.8 KB; pthread 3.4 KB; malloc 3.1 KB; the rest 36.5 KB.

The 1.4 MB of `.bss` (the manager's static tables: `G` 538 KB, the manifest checker's
`check_set.all` 295 KB, the wiring store 85 KB, `S`, `C`, `M`) costs no file bytes; it
bounds the resident memory only as far as it is touched (docs/00's other target, under
2 MB resident with 20 modules).

## What brings it down, measured

| Build | Bytes |
|---|---|
| `-O3` (the release) | 292,784 |
| `-Os` | 250,640 |
| `-O3`, no unwind tables (`-fno-asynchronous-unwind-tables -fno-unwind-tables`) | 259,832 |
| `-Os`, no unwind tables | 218,656 |
| `-Os`, no unwind tables, `-ffunction-sections -fdata-sections -Wl,--gc-sections` | **187,192** |

Flags alone take a third off and need no code change: hangard is event driven and spends
its time asleep in `epoll`, so `-Os` costs it nothing measurable; the unwind tables serve
no C code here (no exceptions; a backtrace in a crash is not the manager's). The flags
could be the manager's alone, leaving `-O3` to the modules' hot paths.

The rest is libc and the manager's own code:

1. **Number parsing without scanf or strtod** (13.5 KB, and with it most of the 5.8 KB of
   128-bit float runtime): the manifest and wiring parsers read durations, rates and
   sizes; integer parsing with a fixed-point fraction covers what they accept.
2. **No qsort** (11 KB): the tables sorted are small; an insertion sort of a few lines.
3. **Time without the time-zone machinery** (6.5 KB): log stamps in UTC from
   `clock_gettime`, formatted by hand, instead of `localtime`/`strftime`.
4. **Less printf** (8.5 KB): harder, since `snprintf` is everywhere; a small formatter for
   the few conversions used would do, but it is the least worthwhile of the four.
5. **The manifest checker** (`mf_set_check`, `mf_parse_text`, 22 KB at `-O3`): large
   switch-heavy functions that `-Os` already shrinks most.

Estimated together with the flags: about 140–150 KB. **Under 100 KB is not reachable
without removing function.** The spike's 50 KB manager had no manifests, calls,
parameters, health or wiring generation, and docs/00's target was set as "twice what has
been measured" for that subset. A decision for the project: restate the target for the
full manager (for example, under 200 KB with the flags above, under 150 KB with the libc
work), or keep it and move the manifest checking out of the manager (into `hangar
check`, for example).

## Applied (2026-11-20)

`tools/release.sh` builds `hangard` and `hangar` a second time into `<work>/build-small`
with `-Os -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections
-fdata-sections` and `-static -s -Wl,--gc-sections`, and ships those two in place of the
`-O3` ones; every other binary, and `lib/hg` inside the modules, stays `-O3`. If the
second build fails, the `-O3` pair ships and the script says so.

Measured on the branch `params-backup` (main at `8b72be1` plus the parameter backup, which
adds nothing to `hangard`), zig cc 0.17.1, `-mcpu=cortex_a72`:

| Binary (aarch64, musl, static, stripped) | `-O3` | the size build |
|---|---|---|
| `hangard` | 313,920 | **201,040** (`.text` 169,400, `.rodata` 30,034, no unwind tables) |
| `hangar` | 301,792 | 193,776 |
| `hangard`, x86-64 (`release.sh --host`) | | 199,696 |

The manager is 21 KB larger at `-O3` than when this file was written (292,784 at
`bfe3bfb`), and the flags that gave 187,192 then give 201,040 now: **1,040 bytes over the
200 KB target** (in units of 1,024 bytes it is 196.3). No further flag closes that:
`-Oz` gives the same bytes as `-Os` with zig cc, `-fno-stack-protector` and
`-Wl,--build-id=none` change nothing, `-fmerge-all-constants -fno-ident` saves 32 bytes,
`-Wl,-z,norelro` 192, and zig cc's linker does not take `--icf`. The next step is the
first of the list above (number parsing without scanf and strtod, 13.5 KB at `-O3`), or
the insertion sort in place of `qsort` (11 KB).

What was checked with the flags: every suite of `make check` passes built with them by
gcc (glibc), and by zig cc for x86_64-linux-musl, static, as the release is (there
without the suites of `video_relay` and `topside_encoder`, whose libraries were not built
for that target, and after one check in `test_hangard_run` stopped expecting glibc's
words for `EBUSY`). The x86-64 release (`release.sh --host`) ran a `lab.sh` rehearsal
on the laptop: both managers up, ArduSub's parameters saved, set and restored against
the fake ArduSub, down. Of the aarch64 release, `hangar` and three of the `test_hg_*`
suites (layout, wiring, pubsub) were run under QEMU's user mode, which cannot run the
manager. **Not checked: the aarch64 manager on a Pi.**
