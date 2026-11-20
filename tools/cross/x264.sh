#!/bin/sh
# tools/cross/x264.sh: a static libx264, 8-bit, 4:2:0, the library alone (no
# CLI, no lavf/swscale/ffms/gpac/lsmash), for topside_encoder
# (modules/topside_encoder/README.md), built with the target's compiler. Used by
# tools/release.sh and CI; cached: a second run with the prefix already built
# does nothing.
#
#   tools/cross/x264.sh --cc "zig cc -target aarch64-linux-musl -mcpu=cortex_a72" \
#                       --arch aarch64 --dir <work>/x264 [--jobs N]
#
# Prints the prefix on success (<dir>/prefix: include/x264.h, x264_config.h,
# lib/libx264.a). Needs git and make; nasm for x86-64's assembly (without it the
# encoder is C alone, and says so). x264 is GPL-2.0-or-later: LICENSING.md.
#
# The source is fetched by its git commit, which names its content: no tarball
# checksum to drift (code.videolan.org's archives are generated on request).
set -eu

X264_COMMIT=b35605ace3ddf7c1a5d67a2eb553f034aef41d55  # the stable branch, 2025-06-08, recorded 2026-11-20
X264_GIT=https://code.videolan.org/videolan/x264.git

cc=
arch=
dir=
jobs=$(nproc 2>/dev/null || echo 2)
while [ $# -gt 0 ]; do
	case "$1" in
	--cc) shift; cc=$1 ;;
	--arch) shift; arch=$1 ;;
	--dir) shift; dir=$1 ;;
	--jobs|-j) shift; jobs=$1 ;;
	-h|--help) sed -n '2,16p' "$0"; exit 0 ;;
	*) echo "x264.sh: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done
[ -n "$cc" ] && [ -n "$arch" ] && [ -n "$dir" ] || { echo "x264.sh: --cc, --arch and --dir are needed" >&2; exit 2; }

prefix=$dir/prefix
if [ -f "$prefix/lib/libx264.a" ] && [ -f "$prefix/include/x264.h" ]; then
	echo "$prefix"
	exit 0
fi
for tool in git make; do
	command -v "$tool" >/dev/null 2>&1 || { echo "x264.sh: $tool is needed" >&2; exit 1; }
done
mkdir -p "$dir"
src=$dir/src
if [ ! -f "$src/configure" ] || [ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" != "$X264_COMMIT" ]; then
	rm -rf "$src"
	mkdir -p "$src"
	echo "x264.sh: fetching x264 $X264_COMMIT" >&2
	git -C "$src" init -q
	git -C "$src" fetch -q --depth 1 "$X264_GIT" "$X264_COMMIT"
	git -C "$src" checkout -q FETCH_HEAD
	[ "$(git -C "$src" rev-parse HEAD)" = "$X264_COMMIT" ] || { echo "x264.sh: fetched another commit" >&2; exit 1; }
fi

case "$arch" in
x86_64) host=x86_64-linux-musl
	asm=
	command -v nasm >/dev/null 2>&1 || { asm=--disable-asm; echo "x264.sh: no nasm: the x86-64 encoder is C alone" >&2; } ;;
aarch64) host=aarch64-linux-musl; asm= ;;
arm) host=arm-linux-musleabihf
	asm=--disable-asm; echo "x264.sh: armv7: the encoder is C alone (its assembly wants an -march configure does not see in zig's flags)" >&2 ;;
*) echo "x264.sh: --arch x86_64, aarch64 or arm" >&2; exit 2 ;;
esac

# x264 builds in its source directory; a copy per build keeps the cached
# source clean for the next target. zig provides ar, ranlib and the assembler
# for aarch64's and arm's .S files (through the C compiler).
build=$dir/build
rm -rf "$build" "$prefix"
cp -R "$src" "$build"
cd "$build"
# shellcheck disable=SC2086
CC="$cc" AR="zig ar" RANLIB="zig ranlib" STRIP=true AS="$cc" \
	./configure --prefix="$prefix" --host="$host" --enable-static --disable-cli \
	--disable-opencl --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash \
	--disable-avs --bit-depth=8 --chroma-format=420 --enable-pic $asm \
	> "$dir/configure.log" 2>&1 || { tail -20 "$dir/configure.log" >&2; exit 1; }
make -j "$jobs" install-lib-static > "$dir/build.log" 2>&1 || { tail -20 "$dir/build.log" >&2; exit 1; }
echo "$prefix"
