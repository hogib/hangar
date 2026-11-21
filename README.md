# hangar

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.24084277.svg)](https://doi.org/10.5281/zenodo.24084277)

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

## What is in it

- **Topics** (`lib/hg`): fixed-size messages in shared memory, a wake-up without
  polling, calls and parameters on the same mechanism, and in every message the
  identifiers of the inputs it was computed from.
- **A manager** (`hangard`) that starts each module from a manifest, wires it to exactly
  the topics it declares, restarts it with back-off, runs an actuator's neutral command
  whenever the actuator's module exits, and adopts the running modules again after its
  own restart. `hangar` is its command-line tool.
- **Vehicle control through ArduSub**: `safety` (arming, failsafes, the gate every
  actuator command passes), `mux` (pilot, controller and bounded policy output per
  axis), `mavlink` (its own MAVLink 2 over UDP or the autopilot's serial line),
  `joystick`, `nav` and `depth_hold`.
- **Video without a transcoder on the vehicle**: `encoder` sends the camera's own H.264
  over RTP; `video_relay`, `vision` and `topside_encoder` decode, process and re-encode
  on the topside.
- **Recording and learning**: `recorder` and its `.hgr` files, dataset specifications,
  the `pairer` that turns provenance into training rows as they happen, `policy` for
  inference on the vehicle with checked weights, and a Python package for reading
  recordings, training and exporting.
- **A tether bridge** (`bridge`): topics and calls between the vehicle and the topside
  over an authenticated UDP link, with the two clocks reconciled.
- **Dawn Patrol** (`console`): a web console for the pilot and the operator.
- **A simulator**: `sim_vehicle`, a 6-DOF model that runs alone or as the physics of
  ArduSub's SITL (`tools/sim/`).

It has been run on a BlueROV2 (Raspberry Pi 5, ArduSub 4.5 on a Pixhawk); the
procedure and the measurements are in [tools/rov/README.md](tools/rov/README.md).

## Layout

| Directory | |
|---|---|
| `docs/` | the design, one document per part; start at [`00-overview.md`](docs/00-overview.md) |
| `lib/hg/` | the module library: topics, provenance, waiting, calls, parameters, recordings (LGPL) |
| `hangard/`, `hangar/` | the manager and the command-line tool |
| `modules/` | the modules, one directory each with its own README, and `common/` for what they share |
| `msg/` | message types (`.msg`) and their generated headers |
| `profiles/` | manifest sets: the simulation roots, the lab's BlueROV2 and its topside |
| `lib/sim/`, `lib/nn/` | the simulator's physics; the inference runtime and the `.hgw` weights file |
| `lib/vision/`, `lib/camera/`, `lib/net/`, `lib/video/`, `lib/proc/`, `lib/common/` | image operators, camera, network and video code, much of it carried over from electric-eye |
| `python/` | the topside package: schemas, `.hgr` and dataset readers, live topics, training and export |
| `tools/` | the message and MAVLink generators, the release and cross builds, the simulator container, and `tools/rov/` for a session on a real vehicle |
| `bench/` | the benchmark harness and its results |
| `legacy/` | electric-eye's `eeye` and `hellion`, kept until the native video modules have replaced them |
| `proposal/` | the research proposal |

## Building

C11, GNU make, and gcc, clang or `zig cc`; libjpeg-turbo for the camera code. The video
modules that decode and encode need libavcodec and libx264 and are left out where
those are missing.

```sh
make -j"$(nproc)" all        # the manager, the tool and every module
make -j"$(nproc)" check      # every test suite
make check-tsan check-asan   # the library's suites under the sanitizers
make equivalence             # image operators hashed against electric-eye v0.3.1
tools/release.sh             # the static aarch64 release tarball (zig cc, musl)
```

Static release binaries are built with `zig cc` for `aarch64-linux-musl` and
`arm-linux-musleabihf`; see [tools/cross/README.md](tools/cross/README.md). CI runs
the suites with gcc, clang and zig cc on x86-64 and natively on arm64, the sanitizers,
the equivalence check, and the cross builds.

## Running it

- In simulation, with Gazebo and ArduSub's SITL in a container:
  [tools/sim/SETUP.md](tools/sim/SETUP.md).
- On a vehicle, from the topside laptop over the tether:
  [tools/rov/QUICKSTART.md](tools/rov/QUICKSTART.md). A session saves the autopilot's
  parameters before it changes any and puts them back at the end.
- The command-line tool and the console: [docs/12-tools.md](docs/12-tools.md).

## Citing hangar

If you use hangar in your research, please cite it. The metadata is in
[CITATION.cff](CITATION.cff) (GitHub's "Cite this repository" gives it as APA and
BibTeX). Each release is archived on Zenodo:
[10.5281/zenodo.24084277](https://doi.org/10.5281/zenodo.24084277) names all versions
and resolves to the latest; each version has a DOI of its own on that record.

> H. Oğuz Bolat (Department of Geomatics Engineering, Yıldız Technical University;
> ORCID [0009-0000-8522-7746](https://orcid.org/0009-0000-8522-7746)) and Hakan Ersoy
> (Department of Electronics and Communications Engineering, Yıldız Technical
> University; ORCID [0009-0006-0495-6589](https://orcid.org/0009-0006-0495-6589)).
> *hangar: a lightweight module system for small underwater vehicles, with a
> provenance-recorded learning loop.* Software, version 0.1.3, 2026.
> https://doi.org/10.5281/zenodo.24084277

```bibtex
@software{bolat_ersoy_hangar_2026,
  author  = {Bolat, H. O{\u{g}}uz and Ersoy, Hakan},
  title   = {hangar: a lightweight module system for small underwater vehicles,
             with a provenance-recorded learning loop},
  version = {0.1.3},
  year    = {2026},
  doi     = {10.5281/zenodo.24084277},
  url     = {https://github.com/hogib/hangar},
  license = {GPL-3.0-or-later}
}
```

## Licence

GNU GPL v3.0 or later for the system; GNU LGPL v3.0 or later for the module library
(`lib/hg`) and the Python package, so that a module may be written in any language
under any licence. electric-eye's code keeps its MIT notices. See
[LICENSING.md](LICENSING.md).
