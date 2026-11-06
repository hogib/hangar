# Licensing

hangar is free software under two licenses, split by what a piece of code is for.

| Part | License | File |
|---|---|---|
| The system: `hangard`, the `hangar` tool, every shipped module (safety, mux, bridge, recorder, pairer, policies, camera, vision, encoder, MAVLink, simulators), the installer, the benchmarks | **GNU GPL v3.0 or later** | [LICENSE](LICENSE) |
| The module library (the `hg_*` API every module links, in whichever language it is written), the `.msg` generator's run-time support, and the Python package | **GNU LGPL v3.0 or later** | [LICENSE.LGPL](LICENSE.LGPL) (a set of permissions added to the GPL; both texts apply) |
| Documentation (`docs/`, `proposal/`) | GPL v3.0 or later, as the code | |

The split is deliberate. The system is copyleft, so an improvement to the manager or
the learning loop is shared back. The library is LGPL, so a module may be written in any
language and under any license, free or not, and still link it; hangar is meant to be
usable by people who are not willing to put their own modules under the GPL. The file
formats and protocols the docs specify are not covered by any license: a reader or
writer written from the docs owes nothing.

Each source file carries an SPDX line as its first comment:

```
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-License-Identifier: LGPL-3.0-or-later
```

and a `LICENSE` or `LICENSE.LGPL` file sits at the root of each component's directory
where that differs from the repository's.

## Third-party code

| Code | License | Linked into |
|---|---|---|
| electric-eye (`eeye`, `hellion`; Oğuz Bolat) | MIT | camera, vision, encoder, video relay, console; its notices are kept |
| musl | MIT | every static binary |
| libjpeg-turbo | BSD-3 / IJG / zlib | camera, vision |
| libx264 | GPL-2.0-or-later | encoder only |
| FFmpeg / libavcodec | LGPL-2.1-or-later (GPL as built with x264) | video relay (topside); encoder's MP4 child process |
| BLAKE2 reference implementation | CC0 | bridge |
| minisign verification (monocypher) | BSD-2 / CC0 | installer, manager |
| MAVLink generated headers | MIT | MAVLink modules |
| Rust standard library, if Rust is chosen | MIT / Apache-2.0 | every binary |

ArduSub (GPL-3.0), QGroundControl (Apache-2.0 / GPL-3.0) and the Linux kernel run as
separate programs and are not linked.

## Binaries and source

Release tarballs and Debian packages contain statically linked binaries. The source
that corresponds to each release is the tagged commit named in the release `MANIFEST`,
at the repository URL given there; the `.deb` names it in its `Source:` and
`Vcs-Git:` fields ([docs/13-deployment.md](docs/13-deployment.md)).
