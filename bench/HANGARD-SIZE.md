# hangard's size

docs/00 sets the manager's target at **under 100 KB** stripped and static, twice the
50 KB of the language spike's manager (topics, spawning, pidfds, the registry and the
control socket). The release's `hangard` is **292,784 bytes** (aarch64, musl, zig cc 0.17,
`-O3`, `-static -s`; 2026-11-20, main at `bfe3bfb`). This is where the bytes are, and what
would bring them down. Measured only: nothing in `hangard/` was changed.

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
