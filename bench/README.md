# bench: hangar measured

`hg_bench` is hangar's side of the benchmark of [docs/12](../docs/12-tools.md#the-benchmark):
the targets of [docs/00](../docs/00-overview.md#targets), measured on the committed
module library (`lib/hg`) and nothing else. The ROS 2 side, a talker/listener pair
measured the same way on the same machine, is `bench/ros2/` (to be written). The
first run on real hardware is planned in [PI-TEST.md](PI-TEST.md).

| File | |
|---|---|
| `hg_bench.c` | the harness (C11, GPL-3.0-or-later); each measurement's method is the comment above its function |
| `bench.mk` | GNU make fragment; the root Makefile includes it (`make bench`, `make bench-quick`) |
| `pi/manifests/` | `eeye.module`, `hellion.module`: electric-eye as phase-A legacy modules ([docs/11](../docs/11-electric-eye.md)) |
| `pi/hangard.conf`, `pi/install-root.sh` | a manager run out of one directory, `$HANGAR_ROOT` |
| `PI-TEST.md` | the Pi visit, step by step |
| `ros2/` | placeholder: the ROS 2 scripts and the comparison script |

## Building

```sh
make bench                      # builddir/hg_bench, with the root Makefile's CC and flags
make bench-quick                # builds it and runs every measurement briefly
# release build, static musl, as the tarball ships it:
tools/release.sh                # out/release/hangar-<version>-aarch64.tar.zst, bin/hg_bench inside
tools/release.sh --host         # the same for this machine
# on its own, against another checkout of lib/hg:
make -f bench/bench.mk BUILDDIR=/tmp/b HG_DIR=/path/to/lib/hg bench
```

It links `lib/hg/hg_topic.c`, `hg_type.c` and `hg_wiring.c` and needs libc, `-lrt`,
`-pthread` and a Linux 5.16 kernel (`futex_waitv`). `HG_COMMIT`, lib/hg's last
commit, is passed with `-D` by `bench.mk` and goes into the report.

## Running

```sh
hg_bench all --json report.json --bin builddir/hangard --bin builddir/eeye
hg_bench publish latest                    # one or more subcommands
hg_bench wake --seconds 60                 # longer wake runs
hg_bench sizes bin/*                       # sizes alone take their files as arguments
hg_bench all --quick                       # short runs of everything: a smoke test, not numbers
```

| Subcommand | Measures | Default length |
|---|---|---|
| `publish` | ns per `hg_publish`, no reader, payloads 16 B, 256 B, 4 KB, 1 MB | median of 5 runs of 1 M (1 MB: 1 M/256) |
| `latest` | ns per `hg_latest` of the same sizes (publish+read loop minus publish loop) | same |
| `wake` | publish to reader wake-up through `hg_wait`, writer and reader separate processes pinned to two CPUs, 100 Hz, 1 kHz, 10 kHz: p50, p99, max, wakes, CPU, context switches | 20 s each |
| `wake_nopin` | the 1 kHz run again with no pinning | 20 s |
| `idle` | a reader asleep in `hg_wait`: context switches and CPU ticks from `/proc`, must be 0 | 30 s |
| `rss` | the 1 kHz pair's Rss, Pss (anon, file, shmem) and VmHWM from `/proc/<pid>/smaps_rollup` and `status` | 3 s |
| `sizes` | `st_size` of each `--bin`, and whether the ELF is static and stripped | |
| `torn` | the spike's torn-read test: 4 readers against a 4-slot ring, torn copies must be 0 | 3 s |
| `lost_wake` | the spike's lost-wake test: 20 000 messages at random 20–200 µs gaps, timeouts must be 0 | ~2 s |
| `all` | everything above (`sizes` only with `--bin`) | ~3 min |

Options: `--runs N`, `--count N`, `--seconds N`, `--idle-seconds N`, `--torn-seconds N`,
`--lost-msgs N`, `--no-pin`, `--quick`, `--json <file>`, `--bin <path>`. The topic
directory is `$HANGAR_TOPIC_DIR` if set, else a fresh `/dev/shm/hg-bench-<uid>-<pid>`,
removed at exit. Run it on a quiet machine: with other load, the `wake` tails say more
about the scheduler than about hangar (compare `wake` and `wake_nopin`).

## Reading the JSON

One object, one key per section, written only for the sections that ran:

```
hg_bench     hg_commit, compiler, args
date         UTC, ISO 8601
machine      arch, kernel, cpu (/proc/cpuinfo), model (/proc/device-tree/model, Pis), cpus, governor, topic_dir
publish      runs, sizes[]: bytes, count, ns (the median), raw[] (every run)
latest       as publish, plus no_message (reads that returned nothing: must be 0)
wake[]       per rate: rate_hz, seconds, writer_cpu/reader_cpu (-1: unpinned), expected,
             published, msgs, wakes, timeouts, gaps, torn, writer_late, kept_up,
             samples, p50_us, p99_us, max_us, mean_us, writer/reader_cpu_s and _pct,
             writer/reader_ctxsw [voluntary, involuntary]
wake_nopin   one such object, 1 kHz, unpinned
idle         seconds, vol/nonvol context switches and cpu_ticks at t0 and t1, their deltas
             (must be 0), rss_kb, wakes_total (1: the close that ends the run), ok
rss          the 1 kHz pair's object with writer{} and reader{}: rss_kb, pss_kb,
             pss_anon_kb, pss_file_kb, pss_shmem_kb, hwm_kb
sizes[]      path, bytes, elf, static, stripped
torn         seconds, readers, slots, published, msgs_read, torn (0), busy, gaps, ok
lost_wake    sent, msgs (= sent), gaps (0), timeouts (0), torn (0), wakes, ok
```

The targets of docs/00 map onto: `publish.sizes[bytes=256].ns` (under 1000 on a Pi 4),
`latest.sizes[bytes=256].ns` (under 500), `wake[rate_hz=1000].p50_us/p99_us`
(reported), `idle.*_delta` (0), `sizes[].bytes` of `hangard` (under 100 KB). The
spike's numbers for the same loops on x86-64 are `spike/RESULTS.json`
(`publish_ns`, `publish16_ns`, `latest_ns`, `latency_under_manager_1kHz_256B_20s`,
`idle_manager_30s`, `rss_kb_at_1kHz`); the harness's `publish` is a few ns slower than
the spike's because the library now writes provenance (four input references) into
every slot.

A quick look:

```sh
jq '.publish.sizes[] | {bytes, ns}' report.json
jq '.wake[] | {rate_hz, p50_us, p99_us, max_us, gaps, writer_late, kept_up}' report.json
jq '{idle: .idle.ok, torn: .torn.ok, lost_wake: .lost_wake.ok}' report.json
```

## The ROS 2 side (placeholder)

`bench/ros2/` will hold a talker and a listener (rclcpp, default middleware) for the
same message sizes and rates, a script that runs them in a container on the comparison
machine and collects the same lines (resident memory, CPU at idle and at rate, latency,
publish cost, sizes, start), and `compare.py`, which takes hangar's report and the
ROS 2 report and prints the factor per line, the pass/fail of docs/12. Until then,
the hangar report alone is what the Pi visit brings home.
