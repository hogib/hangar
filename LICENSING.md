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
| libx264 | GPL-2.0-or-later (combined with hangar's GPL-3.0-or-later under the "or later") | `topside_encoder` (topside), statically: x264's library alone, 8-bit, 4:2:0, built by `tools/cross/x264.sh` from the stable branch's commit `b35605ace3ddf7c1a5d67a2eb553f034aef41d55` (fetched by that commit, which names its content) with `--enable-static --disable-cli --disable-opencl --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash --disable-avs --bit-depth=8 --chroma-format=420`; the corresponding source is that commit and hangar's own source, released with each binary |
| FFmpeg / libavcodec, libavutil | LGPL-2.1-or-later (no GPL or nonfree part configured) | `video_relay` (topside), statically: FFmpeg 7.1.1's H.264 decoder and parser alone, built by `tools/cross/avcodec.sh` with `--disable-everything --enable-decoder=h264 --enable-parser=h264 --enable-parser=hevc --disable-programs --disable-doc --disable-network --disable-autodetect --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample --disable-swscale --enable-static --disable-shared`; the source is the pinned release tarball named there, and hangar's own source, released with each binary, is what relinking against a modified libavcodec needs |
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
