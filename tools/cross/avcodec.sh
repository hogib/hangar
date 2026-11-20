#!/bin/sh
# tools/cross/avcodec.sh: a static libavcodec (and libavutil) with FFmpeg's
# H.264 decoder and parser alone, for video_relay (modules/video_relay/README.md),
# built with the target's compiler. Used by tools/release.sh and CI; cached:
# a second run with the prefix already built does nothing.
#
#   tools/cross/avcodec.sh --cc "zig cc -target aarch64-linux-musl -mcpu=cortex_a72" \
#                          --arch aarch64 --dir <work>/avcodec [--jobs N]
#
# Prints the prefix on success (<dir>/prefix: include/, lib/libavcodec.a,
# lib/libavutil.a). Needs curl, xz, make; nasm for x86-64's assembly (without it
# the decoder is C alone, and says so). FFmpeg is LGPL 2.1 or later as
# configured here (no GPL parts, no nonfree): LICENSING.md lists the flags.
set -eu

FF_VERSION=7.1.1
FF_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1  # recorded 2026-11-20 from ffmpeg.org
FF_URL="https://ffmpeg.org/releases/ffmpeg-$FF_VERSION.tar.xz"

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
	-h|--help) sed -n '2,14p' "$0"; exit 0 ;;
	*) echo "avcodec.sh: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done
[ -n "$cc" ] && [ -n "$arch" ] && [ -n "$dir" ] || { echo "avcodec.sh: --cc, --arch and --dir are needed" >&2; exit 2; }

prefix=$dir/prefix
if [ -f "$prefix/lib/libavcodec.a" ] && [ -f "$prefix/lib/libavutil.a" ] && [ -f "$prefix/include/libavcodec/avcodec.h" ]; then
	echo "$prefix"
	exit 0
fi
for tool in curl xz make; do
	command -v "$tool" >/dev/null 2>&1 || { echo "avcodec.sh: $tool is needed" >&2; exit 1; }
done
mkdir -p "$dir"
tarball=$dir/ffmpeg-$FF_VERSION.tar.xz
if [ ! -f "$tarball" ]; then
	echo "avcodec.sh: downloading FFmpeg $FF_VERSION" >&2
	curl -fsSL -o "$tarball.part" "$FF_URL" && mv "$tarball.part" "$tarball"
fi
got=$(sha256sum "$tarball" | cut -d' ' -f1)
if [ "$got" != "$FF_SHA256" ]; then
	echo "avcodec.sh: $tarball: SHA-256 $got, expected $FF_SHA256" >&2
	exit 1
fi
src=$dir/src
if [ ! -f "$src/configure" ]; then
	rm -rf "$src"
	mkdir -p "$src"
	tar -xJf "$tarball" -C "$src" --strip-components=1
fi

asm=
case "$arch" in
x86_64) command -v nasm >/dev/null 2>&1 || { asm=--disable-x86asm; echo "avcodec.sh: no nasm: the x86-64 decoder is C alone" >&2; } ;;
aarch64|arm) ;;
*) echo "avcodec.sh: --arch x86_64, aarch64 or arm" >&2; exit 2 ;;
esac

# zig provides ar and ranlib; nm (configure's symbol checks) is the host's.
# --enable-parser=hevc only for a build bug of FFmpeg 7.1.1: the H.264 SEI code
# (h2645_sei.c) calls ff_aom_uninit_film_grain_params, whose aom_film_grain.o
# is built only with hevc_sei, which the HEVC parser selects (no HEVC decoder).
build=$dir/build
rm -rf "$build" "$prefix"
mkdir -p "$build"
cd "$build"
# shellcheck disable=SC2086
"$src/configure" --prefix="$prefix" --enable-cross-compile --target-os=linux --arch="$arch" \
	--cc="$cc" --ar="zig ar" --ranlib="zig ranlib" --nm=nm \
	--disable-everything --enable-decoder=h264 --enable-parser=h264 --enable-parser=hevc \
	--disable-programs --disable-doc --disable-network --disable-autodetect \
	--disable-avdevice --disable-avformat --disable-avfilter --disable-swresample --disable-swscale \
	--enable-static --disable-shared --disable-debug --enable-pthreads $asm \
	> "$dir/configure.log" 2>&1 || { tail -20 "$dir/configure.log" >&2; exit 1; }
make -j "$jobs" install > "$dir/build.log" 2>&1 || { tail -20 "$dir/build.log" >&2; exit 1; }
echo "$prefix"
