# Cross builds with zig cc

hangar's release binaries are static musl builds made with `zig cc`, which bundles
musl for every target and needs no installed cross toolchain
([docs/00-overview.md](../../docs/00-overview.md)). The same toolchain builds
electric-eye, hangar's first module.

| Target | `-target` | `-mcpu` |
|---|---|---|
| Raspberry Pi 4/5, 64-bit | `aarch64-linux-musl` | `cortex_a76` (Pi 5; runs on the Pi 4's A72) |
| Raspberry Pi 2-4, 32-bit | `arm-linux-musleabihf` | `cortex_a53` (NEON + VFPv4) |
| Desktop, CI | `x86_64-linux-musl` | default |

## libjpeg-turbo for aarch64

```sh
curl -fsSL -o ljt.tar.gz https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.2.0.tar.gz
tar -xzf ljt.tar.gz
cmake -S libjpeg-turbo-3.2.0 -B ljt-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=tools/cross/zig-aarch64-musl.cmake \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
  -DWITH_TURBOJPEG=ON -DWITH_JPEG8=ON
ninja -C ljt-build turbojpeg-static        # ljt-build/libturbojpeg.a
```

No `nasm` is needed on aarch64 (its SIMD is intrinsics); x86-64 builds of
libjpeg-turbo with SIMD do need it.

## electric-eye (`legacy/eeye`, `legacy/hellion`), static aarch64

The root `Makefile` builds the library code under `lib/` and the legacy programs under
`legacy/`:

```sh
make BUILD=release BUILDDIR=out/aarch64 \
  CC="zig cc -target aarch64-linux-musl -mcpu=cortex_a76" HOSTCC="zig cc" \
  LDFLAGS=-static \
  TURBOJPEG_CFLAGS="-Iljt-src/libjpeg-turbo-3.2.0/src -Iljt-build" \
  TURBOJPEG_LIBS="ljt-build/libturbojpeg.a"
# the test suites, run under qemu user-mode emulation:
make ... TIMEOUT="qemu-aarch64 -0" check
# the operators' hashes against electric-eye v0.3.1, NEON paths included
# (tools/equivalence/README.md):
make ... TIMEOUT="qemu-aarch64 -0" equivalence
```

OpenMP is not available in static musl builds (zig ships no OpenMP runtime), so the
operator loops run single-threaded there; the Makefile detects this and says so.
Natively, `zig cc` uses LLVM's libomp when it is installed (`-lomp`, detected by the
Makefile's probe), and gcc its own libgomp.

Measured 2026-11-07, stripped: `eeye` 1.09 MB, `hellion` 1.22 MB (static, with musl
and libjpeg-turbo). The NEON path is compiled in (`GS_NEON_AARCH64`), and all 17
suites pass under `qemu-aarch64`.
