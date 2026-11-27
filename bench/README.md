# bench: hangar measured

`hg_bench` is hangar's side of the benchmark of [docs/12](../docs/12-tools.md#the-benchmark):
the targets of [docs/00](../docs/00-overview.md#targets), measured on the committed
module library (`lib/hg`) and nothing else. The ROS 2 side, a talker/listener pair
measured the same way on the same machine, is [`bench/ros2/`](ros2/README.md). The
first run on real hardware is planned in [PI-TEST.md](PI-TEST.md).

| File | |
|---|---|
| `hg_bench.c` | the harness (C11, GPL-3.0-or-later); each measurement's method is the comment above its function |
| `bench.mk` | GNU make fragment; the root Makefile includes it (`make bench`, `make bench-quick`) |
| `pi/manifests/` | `eeye.module`, `hellion.module`: electric-eye as phase-A legacy modules ([docs/11](../docs/11-electric-eye.md)) |
| `pi/hangard.conf`, `pi/install-root.sh` | a manager run out of one directory, `$HANGAR_ROOT` |
| `PI-TEST.md` | the Pi visit, step by step |
| `e2e-depth-ff/` | the learning loop end to end in simulation ([README](e2e-depth-ff/README.md)): docs/09's adaptive depth hold, run and measured |
| `ros2/` | the ROS 2 baseline: a talker/listener package, `run.sh`, which runs it and `hg_bench` and writes the comparison ([its README](ros2/README.md)) |
| `pairs/` | every system on equal terms: one node harness (`pair.h`) that hangar's node and each baseline's include, a driver that runs a plan of cells over a registry of systems in interleaved rounds, and a report with the statistics a referee asks for ([its README](pairs/README.md) is the protocol). The comparison of the next lab session is taken with it; `hg_bench` and `ros2/` stay as what produced the earlier figures |

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
hg_bench inplace                           # copying against in place, the four sizes, one process
hg_bench wake --wake-bytes 1048576 --wake-rate 30 --in-place   # the frame pair, in place
```

| Subcommand | Measures | Default length |
|---|---|---|
| `publish` | ns per `hg_publish`, no reader, payloads 16 B, 256 B, 4 KB, 1 MB | median of 5 runs of 1 M (1 MB: 1 M/256) |
| `latest` | ns per `hg_latest` of the same sizes (publish+read loop minus publish loop) | same |
| `wake` | publish to reader wake-up through `hg_wait`, writer and reader separate processes pinned to two CPUs, 100 Hz, 1 kHz, 10 kHz: p50, p99, p99.9, max at the wake-up and again with the message copied out, wakes, CPU, context switches | 20 s each |
| `wake_nopin` | the 1 kHz run again with no pinning | 20 s |
| `idle` | a reader asleep in `hg_wait`: context switches and CPU ticks from `/proc`, must be 0 | 30 s |
| `rss` | the 1 kHz pair's Rss, Pss (anon, file, shmem) and VmHWM from `/proc/<pid>/smaps_rollup` and `status` | 3 s |
| `sizes` | `st_size` of each `--bin`, and whether the ELF is static and stripped | |
| `torn` | the spike's torn-read test: 4 readers against a 4-slot ring, torn copies must be 0 | 3 s |
| `lost_wake` | the spike's lost-wake test: 20 000 messages at random 20–200 µs gaps, timeouts must be 0 | ~2 s |
| `all` | everything above (`sizes` only with `--bin`) | ~3 min |
| `inplace` | not part of `all`. Per size, in one process and in turn within each run: `hg_publish` against `hg_loan` + `hg_commit` (nothing written to the slot: the protocol alone), `hg_latest` against `hg_latest_view` + `hg_view_done` (payload not read), and both reads again with every word of the payload summed. Reads are timed one by one, the clock's own cost taken off, so `latest` here is not `latest`'s figure to the nanosecond: compare within a row | median of 5 runs of 1 M (1 MB: 1 M/256) |

Options: `--runs N`, `--count N`, `--seconds N`, `--idle-seconds N`, `--torn-seconds N`,
`--lost-msgs N`, `--no-pin`, `--quick`, `--json <file>`, `--bin <path>`, and for the
wake pair `--wake-bytes N` (its payload, 256 unless given: one of publish's sizes) and
`--wake-rate HZ` (that rate alone: `--wake-bytes 1048576 --wake-rate 30` is a camera's
frames), and `--in-place`: the writer fills a loaned slot and commits it, the reader
takes a view, checks every word in the ring and asks `hg_view_done`
(docs/01 "In place"). Every wake run also prints, and reports as `read_*`, the time to
the message read through, every word checked: with a copy that is the copy and a pass
over it, in place the pass over the ring and the answer that the view was whole. The topic
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
             samples, p50_us, p99_us, p999_us, max_us, mean_us (publish to the reader's
             wake-up), copied_p50_us, _p99_us, _p999_us, _max_us (publish to the message
             copied out by hg_next), payload, writer/reader_cpu_s and _pct,
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
(reported), `idle.*_delta` (0), `sizes[].bytes` of `hangard` (reported; a guide figure of about 200 KB, docs/00). The
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

## On a running vehicle: `procbench.py`

`sudo python3 -I bench/procbench.py [seconds]` measures what hangar, electric-eye and the
MAVLink relay cost while they run: CPU per process (percent of one core), wake-ups per
second (voluntary context switches, every thread) and PSS, once a second, with the machine's load and temperature, from `/proc` and `/sys` alone. It is how
the busy loops of hangar#1 were found on the lab's BlueROV2.

## The ROS 2 side

[`bench/ros2/`](ros2/README.md) holds a talker and a listener (rclcpp) for the same
message sizes and rates, and `run.sh`, which builds them against the ROS 2 installed on
the machine (or in a container), runs them through every middleware it finds and
`hg_bench` beside them on the same two CPUs, and writes one JSON and one table: each
line for hangar and for ROS 2, with the factor, the pass/fail of docs/12. It is not part
of `make all` or `make check`. Not yet run on the Pi: until then, the hangar report alone
is what a visit has brought home.
