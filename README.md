# hangar

A lightweight module system for small remotely operated underwater vehicles, native
to Linux and small enough for the vehicle's own Raspberry Pi. Every function of the
vehicle is its own process; one manager, `hangard`, starts, watches and restarts them;
they exchange fixed-size messages through shared-memory topics in which every message
records, by identifier, the input messages it was computed from. That record is what
makes the **learning loop** possible: training samples defined once and streamed live
to a trainer on the topside, weights sent back and checked by the vehicle before they
run, and any thruster command traceable to the model that produced it, the rows that
trained the model, and the sensor readings behind the rows.

It descends from [electric-eye](legacy/), whose camera, image operators and video
transport it carries forward, and it is built for one vehicle and its topside, not a
fleet: no DDS, no runtime type system, no daemon between publisher and subscriber.

**Status (November 2026):** the design is complete and reviewed
([docs/00-overview.md](docs/00-overview.md) onward, fifteen documents); the topic
library with calls and parameters, the manager, the message-type generator, the
simulator's physics, the inference runtime, the benchmark harness, the web console
and the phase-1 modules (`safety`, `mux`, `mavlink`) are implemented and tested, the
last of them against a fake ArduSub and the Gazebo + ArduSub SITL container of
`tools/sim/`; the first test on a vehicle's Pi is planned
([bench/PI-TEST.md](bench/PI-TEST.md)). Nothing has run on a vehicle yet. The
recorder, the tether bridge and the learning loop are next.

## Layout

| Directory | |
|---|---|
| `docs/` | the design, one document per part; start at `00-overview.md` |
| `lib/hg/` | the module library: topics, provenance, waiting, wiring (LGPL) |
| `lib/vision/`, `lib/camera/`, `lib/net/`, `lib/video/`, `lib/proc/` | code carried over from electric-eye |
| `lib/common/` | shared headers and the thread pool |
| `lib/sim/` | the simulator's physics: a 6-DOF model, the T200 thrust data, the fit |
| `lib/nn/` | the inference runtime and the `.hgw` weights file |
| `hangard/`, `hangar/` | the manager and the command-line tool |
| `modules/` | the modules: `safety`, `mux`, `mavlink` (vehicle and topside), `console` (Dawn Patrol) |
| `profiles/` | manifest sets for the two simulation roots |
| `msg/` | message types (`.msg`) and their generated headers |
| `tools/msggen/`, `tools/mavgen/`, `tools/equivalence/`, `tools/cross/`, `tools/release.sh`, `tools/sim/` | the message generator, the MAVLink generator, the operator-equivalence check against electric-eye v0.3.1, cross-build recipes, the release build, the simulator container |
| `bench/` | the benchmark harness and the Pi test plan |
| `legacy/` | electric-eye's `eeye` and `hellion`, run unchanged as legacy modules until their replacements exist |
| `python/` | the topside package: schema parsing, the `.hgw` writer, the PyTorch exporter |
| `spike/`, `literature/`, `proposal/` | the language spike, the literature survey (not in git), the research proposal |

## Building

C11, GNU make, and either gcc, clang or `zig cc`; libjpeg-turbo for the camera code.

```sh
make -j"$(nproc)"            # eeye, hellion, hangard, hangar
make -j"$(nproc)" check      # every test suite
make equivalence             # operators hashed against electric-eye v0.3.1
make check-tsan check-asan   # the library's suites under the sanitizers
tools/release.sh             # the static aarch64 release tarball (zig cc, musl)
```

Static release binaries are built with `zig cc` for `aarch64-linux-musl` and
`arm-linux-musleabihf`; see [tools/cross/README.md](tools/cross/README.md). CI runs
the suites with gcc, clang and zig cc on x86-64 and natively on arm64, the sanitizers,
the equivalence check, and the cross builds.

## Licence

GNU GPL v3.0 or later for the system; GNU LGPL v3.0 or later for the module library
(`lib/hg`) and the Python package, so that a module may be written in any language
under any licence. electric-eye's code keeps its MIT notices. See
[LICENSING.md](LICENSING.md).
