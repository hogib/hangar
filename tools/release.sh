#!/bin/sh
# tools/release.sh: one command, one release tarball.
#
#   tools/release.sh [--host] [--cpu <mcpu>] [--out <dir>] [--work <dir>]
#                    [--ljt <dir>] [--jobs N]
#
# Produces out/release/hangar-<git describe>-aarch64.tar.zst: every binary a
# static musl build made with `zig cc -target aarch64-linux-musl -mcpu=cortex_a72`
# (the Pi 4's core; a Pi 5 runs it too; --cpu cortex_a76 is the Pi 5 build),
# stripped at link (-s). --host builds the same for this machine
# (x86_64-linux-musl here) as hangar-<version>-x86_64.tar.zst.
#
# What goes in (docs/13-deployment.md "A release", as far as the tree has it):
#   bin/        hangard, hangar, hg_bench, lib/hg's test suites, eeye, hellion,
#               the toy modules of hangard/tests/modules: whatever the root
#               Makefile knows how to build today. A target the Makefile does
#               not have yet is skipped and listed, never an error.
#   share/hangar/modules/   the phase-A manifests (bench/pi/manifests, docs/11)
#   share/hangar/hangard.conf, install-root.sh   a $HANGAR_ROOT run (bench/PI-TEST.md)
#   share/hangar/eeye/, hellion/                 electric-eye's configuration files
#   share/hangar/msg/                            the message types
#   share/doc/                                   PI-TEST.md, LICENSE, LICENSING.md
#   MANIFEST    version, target, commit, the SHA-256 of every file (no signature yet)
#
# libjpeg-turbo 3.2.0 (eeye and hellion link it) is built once for the target
# under <work>/ljt per tools/cross/README.md and reused afterwards; --ljt
# names a directory that already holds build/libturbojpeg.a and src/src/turbojpeg.h.
# Idempotent: a second run rebuilds nothing that is up to date and rewrites
# the tarball. Prints what it built, what it skipped and what `file` says of
# every binary. Needs zig, make, tar, zstd, sha256sum; cmake and curl (plus
# ninja, if present) for libjpeg-turbo, without which eeye and hellion are skipped.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LJT_VERSION=3.2.0
LJT_SHA256=980dd81f425082aa6d7c9e47fef27554ce7a9ffc8e2f6e863b97d263c5c50858  # recorded 2026-11-07 from the GitHub tag tarball
LJT_URL="https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/$LJT_VERSION.tar.gz"

host=no
mcpu=cortex_a72
out=
work=
ljt=
jobs=$(nproc 2>/dev/null || echo 2)
while [ $# -gt 0 ]; do
	case "$1" in
	--host) host=yes ;;
	--cpu) shift; mcpu=$1 ;;
	--out) shift; out=$1 ;;
	--work) shift; work=$1 ;;
	--ljt) shift; ljt=$1 ;;
	--jobs|-j) shift; jobs=$1 ;;
	-h|--help) sed -n '2,33p' "$0"; exit 0 ;;
	*) echo "release.sh: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done

for tool in zig make tar zstd sha256sum; do
	command -v "$tool" >/dev/null 2>&1 || { echo "release.sh: $tool is not installed" >&2; exit 1; }
done

if [ "$host" = yes ]; then
	arch=$(uname -m)
	case "$arch" in
	x86_64) target=x86_64-linux-musl; mcpu= ;;
	aarch64) target=aarch64-linux-musl ;;
	*) echo "release.sh: no musl target for $arch" >&2; exit 1 ;;
	esac
else
	arch=aarch64
	target=aarch64-linux-musl
fi
cc="zig cc -target $target${mcpu:+ -mcpu=$mcpu}"
version=${HANGAR_VERSION:-$(git -C "$ROOT" describe --always --dirty --tags 2>/dev/null || echo unknown)}
commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
source_url=$(git -C "$ROOT" remote get-url origin 2>/dev/null || echo local)
out=${out:-$ROOT/out/release}
work=${work:-$out/work-$arch}
builddir=$work/build
testdir=$builddir/tests-release
name=hangar-$version-$arch
stage=$work/stage/$name
mkdir -p "$out" "$work"

echo "release.sh: $name"
echo "  compiler  $cc"
echo "  work      $work"

# ---- libjpeg-turbo for the target -------------------------------------------
ljt=${ljt:-$work/ljt}
ljt_ok=no
have_ljt() { [ -f "$ljt/build/libturbojpeg.a" ] && [ -f "$ljt/src/src/turbojpeg.h" ]; }
if have_ljt; then
	ljt_ok=yes
	echo "  ljt       $ljt/build/libturbojpeg.a (already built)"
elif command -v cmake >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; then
	mkdir -p "$ljt"
	tarball=$ljt/libjpeg-turbo-$LJT_VERSION.tar.gz
	if [ ! -f "$tarball" ]; then
		echo "  ljt       downloading libjpeg-turbo $LJT_VERSION"
		curl -fsSL -o "$tarball.part" "$LJT_URL" && mv "$tarball.part" "$tarball"
	fi
	got=$(sha256sum "$tarball" | cut -d' ' -f1)
	if [ "$got" != "$LJT_SHA256" ]; then
		echo "release.sh: $tarball: SHA-256 $got, expected $LJT_SHA256" >&2
		exit 1
	fi
	if [ ! -f "$ljt/src/CMakeLists.txt" ]; then
		rm -rf "$ljt/src"
		mkdir -p "$ljt/src"
		tar -xzf "$tarball" -C "$ljt/src" --strip-components=1
	fi
	# The toolchain file of tools/cross/, with this run's target and -mcpu.
	case "$target" in
	aarch64*) proc=aarch64; simd=ON ;;
	x86_64*) proc=x86_64; if command -v nasm >/dev/null 2>&1; then simd=ON; else simd=OFF; fi ;;
	*) proc=$arch; simd=OFF ;;
	esac
	sed -e "s|^set(CMAKE_SYSTEM_PROCESSOR .*|set(CMAKE_SYSTEM_PROCESSOR $proc)|" \
	    -e "s|^set(CMAKE_C_COMPILER .*|set(CMAKE_C_COMPILER $cc)|" \
	    -e "s|/usr/bin/zig|$(command -v zig)|g" \
	    "$ROOT/tools/cross/zig-aarch64-musl.cmake" > "$ljt/toolchain.cmake"
	gen=
	command -v ninja >/dev/null 2>&1 && gen="-G Ninja"
	echo "  ljt       building libturbojpeg.a for $target (SIMD $simd)"
	# shellcheck disable=SC2086
	cmake -S "$ljt/src" -B "$ljt/build" $gen \
	      -DCMAKE_TOOLCHAIN_FILE="$ljt/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
	      -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON -DWITH_JPEG8=ON \
	      -DWITH_SIMD=$simd > "$ljt/cmake.log" 2>&1 || { tail -20 "$ljt/cmake.log"; exit 1; }
	cmake --build "$ljt/build" --target turbojpeg-static -j "$jobs" > "$ljt/build.log" 2>&1 \
		|| { tail -20 "$ljt/build.log"; exit 1; }
	have_ljt && ljt_ok=yes
else
	echo "  ljt       cmake or curl missing: libjpeg-turbo not built, eeye and hellion skipped"
fi

# ---- What the Makefile knows ------------------------------------------------
run_make() {
	# $@: targets. The Makefile's variables, the compiler and libjpeg-turbo.
	make -C "$ROOT" BUILD=release BUILDDIR="$builddir" CC="$cc" HOSTCC="zig cc" \
	     LDFLAGS="-static -s" \
	     TURBOJPEG_CFLAGS="-I$ljt/src/src -I$ljt/build" \
	     TURBOJPEG_LIBS="$ljt/build/libturbojpeg.a" "$@"
}
known() { run_make -n -s "$1" >/dev/null 2>&1; }

built=
skipped=
failed=
targets=
want() {
	# $1: a name for the list, $2: the make target that produces it.
	if known "$2"; then
		targets="$targets $2"
	else
		skipped="$skipped $1"
	fi
}

if [ "$ljt_ok" = yes ]; then
	want eeye "$builddir/eeye"
	want hellion "$builddir/hellion"
else
	skipped="$skipped eeye hellion"
fi
want hangard "$builddir/hangard"
want hangar "$builddir/hangar"
want dawn-patrold "$builddir/dawn-patrold"
for t in "$ROOT"/lib/hg/tests/test_hg_*.c; do
	[ -e "$t" ] || continue
	s=$(basename "$t" .c)
	want "$s" "$testdir/$s"
done
for m in "$ROOT"/hangard/tests/modules/*.c; do
	[ -e "$m" ] || continue
	s=$(basename "$m" .c)
	if known "$builddir/$s"; then targets="$targets $builddir/$s"
	elif known "$builddir/modules/$s"; then targets="$targets $builddir/modules/$s"
	elif known "$testdir/$s"; then targets="$targets $testdir/$s"
	else skipped="$skipped module:$s"; fi
done

echo "  building $(echo "$targets" | wc -w) targets with make -k -j$jobs (log: $work/make.log)"
# -k: a target that fails to build (a tree mid-change) is listed as failed
# below and the rest still ships.
# shellcheck disable=SC2086
run_make -k -j "$jobs" $targets > "$work/make.log" 2>&1 || true

# hg_bench: by the root Makefile once bench/bench.mk is included there, else
# by the fragment on its own.
if known "$builddir/hg_bench"; then
	run_make -k -j "$jobs" "$builddir/hg_bench" >> "$work/make.log" 2>&1 || true
else
	make -f "$ROOT/bench/bench.mk" BUILD=release BUILDDIR="$builddir" CC="$cc" CFLAGS=-O3 \
	     LDFLAGS="-static -s" "$builddir/hg_bench" >> "$work/make.log" 2>&1 || true
fi

# ---- Stage ------------------------------------------------------------------
rm -rf "$work/stage"
mkdir -p "$stage/bin" "$stage/share/hangar/modules" "$stage/share/hangar/eeye" \
	 "$stage/share/hangar/hellion" "$stage/share/hangar/msg" "$stage/share/doc"

put_bin() {
	# $1: the built file, $2: its name in bin/. A known target that is not
	# there after make -k failed to build.
	if [ -x "$1" ]; then
		cp -p "$1" "$stage/bin/$2"
		built="$built $2"
	else
		failed="$failed $2"
	fi
}
for t in $targets; do
	put_bin "$t" "$(basename "$t")"
done
put_bin "$builddir/hg_bench" hg_bench
# A manager or a command the Makefile built under another path: take it if
# it is there, say so if not.
for b in hangard hangar; do
	case " $built " in *" $b "*) continue ;; esac
	f=$(find "$builddir" -maxdepth 3 -type f -perm -u+x -name "$b" -not -path "*/obj-*" 2>/dev/null | head -1)
	if [ -n "$f" ]; then
		cp -p "$f" "$stage/bin/$b"
		built="$built $b"
		skipped=$(echo " $skipped " | sed "s/ $b / /")
	fi
done

cp "$ROOT"/bench/pi/manifests/*.module "$stage/share/hangar/modules/"
cp "$ROOT/bench/pi/hangard.conf" "$stage/share/hangar/hangard.conf"
cp "$ROOT/bench/pi/install-root.sh" "$stage/share/hangar/install-root.sh"
chmod 755 "$stage/share/hangar/install-root.sh"
cp "$ROOT/legacy/eeye/eeye_config.json" "$stage/share/hangar/eeye/effects.json"
cp "$ROOT/legacy/hellion/examples/b-rov.json" "$stage/share/hangar/hellion/hellion.json"
cp "$ROOT/legacy/hellion/examples/b-topside.json" "$stage/share/hangar/hellion/hellion_topside.json"
for m in "$ROOT"/msg/*.msg; do [ -e "$m" ] && cp "$m" "$stage/share/hangar/msg/"; done
cp "$ROOT/LICENSE" "$ROOT/LICENSING.md" "$stage/share/doc/"
[ -f "$ROOT/LICENSE.LGPL" ] && cp "$ROOT/LICENSE.LGPL" "$stage/share/doc/"
[ -f "$ROOT/bench/PI-TEST.md" ] && cp "$ROOT/bench/PI-TEST.md" "$stage/share/doc/"
[ -f "$ROOT/legacy/LICENSE" ] && cp "$ROOT/legacy/LICENSE" "$stage/share/doc/LICENSE.electric-eye"

# ---- MANIFEST ---------------------------------------------------------------
{
	echo "hangar-release 1"
	echo "version $version"
	echo "target $arch $target${mcpu:+ $mcpu}"
	echo "commit $commit"
	echo "source $source_url"
	echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ) zig $(zig version) libjpeg-turbo $LJT_VERSION"
	echo "binaries$built"
	echo "skipped$skipped"
	echo "failed$failed"
	echo
	(cd "$stage" && find . -type f ! -name MANIFEST | sed 's|^\./||' | LC_ALL=C sort | xargs sha256sum)
} > "$stage/MANIFEST"

# ---- Tarball ----------------------------------------------------------------
tarball=$out/$name.tar.zst
tar --zstd -C "$work/stage" --owner=0 --group=0 --numeric-owner --sort=name \
    --mtime="@$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || date +%s)" \
    -cf "$tarball" "$name"

# ---- Report -----------------------------------------------------------------
echo
echo "built:  $built"
echo "skipped:$skipped   (no rule in the Makefile yet)"
[ -z "$failed" ] || echo "FAILED: $failed   (a build error: see $work/make.log)"
echo "binaries ($(du -sh "$stage/bin" | cut -f1) in bin/):"
for b in "$stage"/bin/*; do
	[ -e "$b" ] || continue
	size=$(wc -c < "$b")
	if command -v file >/dev/null 2>&1; then
		what=$(file -b "$b" | sed 's/, BuildID[^,]*//; s/, for GNU\/Linux[^,]*//; s/ELF 64-bit LSB executable, //; s/, version 1 (SYSV)//')
		case "$what" in
		*"statically linked"*) ;;
		*) what="$what  <-- NOT STATIC" ;;
		esac
	else
		what=
	fi
	printf '  %-24s %9d  %s\n' "$(basename "$b")" "$size" "$what"
done
echo "tarball: $tarball ($(du -h "$tarball" | cut -f1))"
