#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/baselines.sh: the baselines of bench/pairs, built here and packed, so that
# nothing is compiled on the Pi during a session (bench/pairs/README.md, "On the
# Pi"; tools/rov/LAB-CHECKLIST.md, "Baselines").
#
#   tools/baselines.sh [--host] [--no-ros2] [--out DIR] [--work DIR]   build and pack
#   tools/baselines.sh ros2-smoke [--image IMAGE] [--reps N] the smoke plan over every
#                                                            system, ROS 2's included,
#                                                            in a container
#
# Produces out/baselines/baselines-<git describe>-aarch64.tar.zst (--host: for this
# machine, for a rehearsal):
#
#   bin/          hangar_pair, null_pair, unix_pair, iceoryx2_pair, and the graph's
#                 modules hangar_graph, unix_graph, iceoryx2_graph: static musl
#                 binaries, every one built from this tree's pair.h with the same
#                 compiler and flags, hangar's own included (a release has
#                 hangar_pair too; the one here shares its harness with the baselines
#                 to the line)
#   bench/pairs/  the driver, the registry, the plans, the ROS 2 node's source and
#                 profiles
#   tools/rov/preflight.py    which the driver runs before each round
#   ros2/install/ the ROS 2 node (ros2_pair_node) and its message libraries, built for
#                 arm64 against ROS 2 Jazzy (see below); ros2-build.sh builds the same
#                 on the machine itself, should the prebuilt one not load there
#   licenses/     of what is linked in or shipped
#   MANIFEST      what each thing is, its version and source, and the SHA-256 of
#                 every file
#
# The ROS 2 node cannot be static: it links the ROS 2 Jazzy of the machine it runs
# on. It is cross-compiled in a container (bench/pairs/ros2_pair/cross: the amd64
# ros:jazzy-ros-base with Ubuntu's aarch64 compiler) against a sysroot that is the
# arm64 ros:jazzy-ros-base image, unpacked and never run; the image's digest is in the
# MANIFEST. Same distribution as the Pi's (noble), so the same glibc and libstdc++;
# the Pi's ROS 2 packages may be of an earlier date than the image's, and whether the
# node loads against them is seen at once (ros2_pair_node --version). If it does not,
# ros2-build.sh builds it there, once, before the session is frozen. Needs docker;
# --no-ros2 leaves it out, and --host always does (a rehearsal builds it in the
# container: ros2-smoke). The .deb files of the middlewares the Pi lacks are fetched
# by tools/baselines-debs.sh, beside this archive.
#
# Third-party source is pinned to a version and a SHA-256, fetched into the work
# directory and built there with its own build, as tools/release.sh does:
#
#   iceoryx2 0.10.1 (Apache-2.0 OR MIT): its C binding, a static library, with cargo
#
# Needs zig, make, cargo with the target's rust-std (rustup target add
# aarch64-unknown-linux-musl; for --host x86_64-unknown-linux-musl), curl, tar, zstd,
# sha256sum. Nothing is installed by this script.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

IOX2_VERSION=0.10.1
IOX2_URL=https://github.com/eclipse-iceoryx/iceoryx2/archive/refs/tags/v$IOX2_VERSION.tar.gz
IOX2_SHA256=f6ad9608f61b93e7578645ef738c3b56ea980761f8765ee47aa49423cb12d763

host=no ros2=yes out= work= image=hangar-ros2-bench:jazzy reps=1 cmd=build
cross_image=hangar-ros2-cross:jazzy
[ "${1:-}" = ros2-smoke ] && { cmd=ros2-smoke; shift; }
while [ $# -gt 0 ]; do
	case $1 in
	--host) host=yes ;;
	--no-ros2) ros2=no ;;
	--out) shift; out=$1 ;;
	--work) shift; work=$1 ;;
	--image) shift; image=$1 ;;
	--reps) shift; reps=$1 ;;
	-h|--help) sed -n '3,47p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "baselines.sh: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done
out=${out:-$ROOT/out/baselines}
mkdir -p "$out"
out=$(cd "$out" && pwd)

# ---- ros2-smoke ---------------------------------------------------------------------
# A proof that every entry of the registry runs, on a machine without ROS 2: the
# container of bench/ros2/Dockerfile, the ROS 2 node built in it, the other nodes
# built here for it (static, so they run in any userland). Never a measurement.
if [ "$cmd" = ros2-smoke ]; then
	command -v docker > /dev/null || { echo "baselines.sh: docker is not installed" >&2; exit 1; }
	docker image inspect "$image" > /dev/null 2>&1 ||
		{ echo "baselines.sh: no image $image: docker build -t $image bench/ros2" >&2; exit 1; }
	"$0" --host --out "$out" ${work:+--work "$work"} > /dev/null
	# The stage just made: the newest, where earlier versions' stages still lie beside it.
	hostdir=$(ls -dt "$out"/work-"$(uname -m)"/stage/baselines-* | head -1)
	ws=$out/ros2-$(uname -m)
	mkdir -p "$ws"
	rm -rf "$ws/smoke" "$ws/build" "$ws/install" "$ws/log" # a workspace remembers its source's path
	docker run --rm --user "$(id -u):$(id -g)" -e HOME=/work --shm-size 2g \
		-v "$hostdir:/b:ro" -v "$ws:/work" "$image" bash -c '
		set -e
		source /opt/ros/jazzy/setup.bash
		cd /work && sh /b/ros2-build.sh /work > /work/ros2-build.log 2>&1 || { tail -30 /work/ros2-build.log; exit 1; }
		source /work/install/setup.bash
		python3 -I /b/bench/pairs/driver.py --plan /b/bench/pairs/plans/smoke.json --reps '"$reps"' \
			--var bin=/b/bin --var base=/b/bin --var ros=/work/install/ros2_pair/lib/ros2_pair \
			--var ros_root=/opt/ros/jazzy --out /work/smoke --no-preflight'
	echo "baselines.sh: $ws/smoke/report.md (a proof of the harness, not a measurement)"
	exit 0
fi

# ---- build --------------------------------------------------------------------------
for tool in zig make cargo curl tar zstd sha256sum; do
	command -v "$tool" > /dev/null 2>&1 || { echo "baselines.sh: $tool is not installed" >&2; exit 1; }
done
if [ "$host" = yes ]; then
	arch=$(uname -m)
	mcpu=
else
	arch=aarch64
	mcpu=cortex_a72 # the Pi 4's core, which a Pi 5 runs too: as tools/release.sh
fi
target=$arch-linux-musl
rust_target=$arch-unknown-linux-musl
cc="zig cc -target $target${mcpu:+ -mcpu=$mcpu}"
rustc --print target-libdir --target "$rust_target" 2> /dev/null | { read -r d; [ -d "$d" ]; } ||
	{ echo "baselines.sh: cargo has no $rust_target: rustup target add $rust_target" >&2; exit 1; }

version=$(git -C "$ROOT" describe --always --dirty --tags 2> /dev/null || echo unknown)
commit=$(git -C "$ROOT" rev-parse HEAD 2> /dev/null || echo unknown)
work=${work:-$out/work-$arch}
mkdir -p "$work"
work=$(cd "$work" && pwd)
builddir=$work/build
name=baselines-$version-$arch
stage=$work/stage/$name
echo "baselines.sh: $name"
echo "  compiler  $cc"
echo "  work      $work"

# hangar's own pairs and the Unix sockets: this tree's Makefile, as a release builds.
make -C "$ROOT" BUILD=release BUILDDIR="$builddir" CC="$cc" HOSTCC="zig cc" LDFLAGS="-static -s" \
	"$builddir/hangar_pair" "$builddir/null_pair" "$builddir/unix_pair" "$builddir/hangar_graph" "$builddir/unix_graph" \
	> "$work/make.log" 2>&1 ||
	{ tail -20 "$work/make.log" >&2; echo "baselines.sh: make failed ($work/make.log)" >&2; exit 1; }
# The flags make gave them, for the pairs that make does not build.
pair_cflags=$(make -C "$ROOT" -s --no-print-directory BUILD=release BUILDDIR="$builddir" CC="$cc" \
	--eval='print-pair-cflags: ; @echo $(HG_CFLAGS) $(CFLAGS)' print-pair-cflags)

# iceoryx2: the pinned source, its C binding as a static library, then the pair.
iox=$work/iceoryx2
mkdir -p "$iox"
if [ ! -f "$iox/src.tar.gz" ] || [ "$(sha256sum < "$iox/src.tar.gz" | cut -d' ' -f1)" != "$IOX2_SHA256" ]; then
	curl -fsSL --retry 3 -o "$iox/src.tar.gz.part" "$IOX2_URL"
	got=$(sha256sum < "$iox/src.tar.gz.part" | cut -d' ' -f1)
	[ "$got" = "$IOX2_SHA256" ] || { echo "baselines.sh: iceoryx2 $IOX2_VERSION: SHA-256 $got, expected $IOX2_SHA256" >&2; exit 1; }
	mv "$iox/src.tar.gz.part" "$iox/src.tar.gz"
	rm -rf "$iox/src"
fi
if [ ! -d "$iox/src" ]; then
	mkdir -p "$iox/src"
	tar -xzf "$iox/src.tar.gz" -C "$iox/src" --strip-components=1
fi
mkdir -p "$iox/wrap"
printf '#!/bin/sh\nexec %s "$@"\n' "$cc" > "$iox/wrap/cc"
printf '#!/bin/sh\nexec zig ar "$@"\n' > "$iox/wrap/ar"
chmod +x "$iox/wrap/cc" "$iox/wrap/ar"
env_target=$(echo "$rust_target" | tr 'a-z-' 'A-Z_')
# A static library only: no linker of the target's is asked for. The header is made
# by the same build (cbindgen), wherever cargo's build script puts it.
(cd "$iox/src" &&
	env "CC_${rust_target//-/_}=$iox/wrap/cc" "AR_${rust_target//-/_}=$iox/wrap/ar" \
		"CARGO_TARGET_${env_target}_LINKER=$iox/wrap/cc" CARGO_TARGET_DIR="$iox/target" \
		cargo rustc --locked --release -p iceoryx2-ffi-c --target "$rust_target" --crate-type staticlib) > "$iox/cargo.log" 2>&1 ||
	{ tail -20 "$iox/cargo.log" >&2; echo "baselines.sh: iceoryx2 did not build ($iox/cargo.log)" >&2; exit 1; }
iox_lib=$iox/target/$rust_target/release/libiceoryx2_ffi_c.a
iox_inc=$(dirname "$(dirname "$(find "$iox/target/$rust_target" -path '*include/iox2/iceoryx2.h' | head -1)")")
[ -f "$iox_lib" ] && [ -f "$iox_inc/iox2/iceoryx2.h" ] ||
	{ echo "baselines.sh: iceoryx2's library or header is not where expected under $iox/target" >&2; exit 1; }
for n in iceoryx2_pair iceoryx2_graph; do
	# shellcheck disable=SC2086
	$cc $pair_cflags -static -s -DIOX2_VERSION="\"$IOX2_VERSION\"" -DPAIR_CFLAGS="\"$cc $pair_cflags\"" \
		-I"$ROOT/bench/pairs" -I"$iox_inc" -o "$builddir/$n" "$ROOT/bench/pairs/$n.c" "$iox_lib" -lunwind \
		> "$iox/link.log" 2>&1 || { cat "$iox/link.log" >&2; echo "baselines.sh: $n did not link" >&2; exit 1; }
done

# The ROS 2 node, cross-compiled against an arm64 ROS 2 Jazzy as a sysroot.
ros2_line="not-built ros2_pair_node: ros2-build.sh builds it on the machine it runs on, against its own ROS 2 Jazzy"
ros2_ws=
if [ "$host" = no ] && [ "$ros2" = yes ]; then
	if ! command -v docker > /dev/null 2>&1; then
		echo "baselines.sh: NOTE no docker: the ROS 2 node is not prebuilt (ros2-build.sh on the Pi)" >&2
	else
		docker image inspect "$cross_image" > /dev/null 2>&1 ||
			docker build -q -t "$cross_image" "$ROOT/bench/pairs/ros2_pair/cross" > "$work/cross-image.log" 2>&1 ||
			{ tail -5 "$work/cross-image.log" >&2; echo "baselines.sh: the cross image did not build" >&2; exit 1; }
		sys=$work/ros2-sysroot
		mkdir -p "$sys"
		if [ ! -d "$sys/root/opt/ros/jazzy" ]; then
			# The arm64 image of the same tag, by digest, so that the local tag stays
			# this machine's own architecture. Created, exported, removed: never run.
			digest=$(docker manifest inspect ros:jazzy-ros-base | python3 -c 'import json, sys
print(next(m["digest"] for m in json.load(sys.stdin)["manifests"] if m["platform"]["architecture"] == "arm64" and m["platform"]["os"] == "linux"))')
			docker pull -q "ros@$digest" > /dev/null
			c=$(docker create "ros@$digest" true 2> /dev/null)
			rm -rf "$sys/root"
			mkdir -p "$sys/root"
			docker export "$c" | tar -x -C "$sys/root" --exclude='dev/*' --exclude='proc/*' --exclude='sys/*' \
				--exclude='var/cache/*' --exclude='usr/share/doc/*' --exclude='usr/share/man/*' 2> "$sys/untar.err" || true
			docker rm "$c" > /dev/null
			[ -d "$sys/root/opt/ros/jazzy" ] || { echo "baselines.sh: the arm64 sysroot has no /opt/ros/jazzy ($sys/untar.err)" >&2; exit 1; }
			echo "$digest" > "$sys/digest"
		fi
		ros2_ws=$work/ros2-ws
		rm -rf "$ros2_ws"
		mkdir -p "$ros2_ws"
		# The multiarch directory is mounted where the sysroot's absolute links point.
		docker run --rm --user "$(id -u):$(id -g)" -e HOME=/work -v "$ROOT:/src:ro" -v "$sys/root:/sysroot:ro" \
			-v "$sys/root/usr/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:ro" -v "$ros2_ws:/work" "$cross_image" bash -c '
			source /opt/ros/jazzy/setup.bash && cd /work &&
			colcon build --base-paths /src/bench/pairs/ros2_pair --cmake-args -DCMAKE_BUILD_TYPE=Release \
				-DPAIR_INCLUDE_DIR=/src/bench/pairs -DBUILD_TESTING=OFF \
				-DCMAKE_TOOLCHAIN_FILE=/src/bench/pairs/ros2_pair/cross/toolchain.cmake' > "$work/ros2-cross.log" 2>&1 ||
			{ tail -20 "$work/ros2-cross.log" >&2; echo "baselines.sh: the ROS 2 node did not cross-build ($work/ros2-cross.log)" >&2; exit 1; }
		node=$ros2_ws/install/ros2_pair/lib/ros2_pair/ros2_pair_node
		file -b "$node" | grep -q 'ARM aarch64' || { echo "baselines.sh: $node is not an aarch64 program" >&2; exit 1; }
		pkgs=$(awk '/^Package: ros-jazzy-(rclcpp|rmw-fastrtps-cpp|fastrtps|rosidl-typesupport-cpp)$/ { p = $2 } /^Version:/ && p { printf "%s %s; ", p, $2; p = "" }' "$sys/root/var/lib/dpkg/status")
		ros2_line="ros2_pair_node: cross-built (gcc $(docker run --rm "$cross_image" aarch64-linux-gnu-gcc -dumpfullversion), -O3) against arm64 ros@$(cat "$sys/digest"): $pkgs"
	fi
fi

# ---- the stage ------------------------------------------------------------------------
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/bench" "$stage/tools/rov" "$stage/licenses/iceoryx2"
bins="hangar_pair null_pair unix_pair iceoryx2_pair hangar_graph unix_graph iceoryx2_graph"
for b in $bins; do cp "$builddir/$b" "$stage/bin/"; done
# The driver and what it reads, the ROS 2 node's source with them; no caches.
(cd "$ROOT" && tar -cf - --exclude=__pycache__ --exclude='*.o' bench/pairs tools/rov/preflight.py) | tar -xf - -C "$stage"
cp "$ROOT/LICENSE" "$stage/licenses/hangar-GPL-3.0-or-later"
cp "$ROOT/LICENSE.LGPL" "$stage/licenses/hangar-lib-LGPL-3.0-or-later"
cp "$iox/src/LICENSE-APACHE" "$iox/src/LICENSE-MIT" "$stage/licenses/iceoryx2/"
[ -f "$iox/src/NOTICE.md" ] && cp "$iox/src/NOTICE.md" "$stage/licenses/iceoryx2/"

if [ -n "$ros2_ws" ]; then
	mkdir -p "$stage/ros2"
	cp -a "$ros2_ws/install" "$stage/ros2/install"
	find "$stage/ros2" -name __pycache__ -prune -exec rm -rf {} +
fi

cat > "$stage/ros2-build.sh" <<'EOF'
#!/bin/sh
# ros2-build.sh [WORKSPACE]: builds bench/pairs' ROS 2 node (ros2_pair) against the
# ROS 2 Jazzy installed on THIS machine, into WORKSPACE (default ~/hangar-baselines-ros2).
# Only needed where the archive has no ros2/install, or where its node does not load
# (try: . /opt/ros/jazzy/setup.sh; . ros2/install/setup.sh; ros2_pair_node --version).
# Then it is the one thing of the baselines compiled where it runs: once, before the
# session is frozen and measured, never between measurements. How long it takes on a
# Pi 5 has not been measured.
#
#   . /opt/ros/jazzy/setup.sh && sh ros2-build.sh
#   then, for the driver:  . WORKSPACE/install/setup.sh
#                          --var ros=WORKSPACE/install/ros2_pair/lib/ros2_pair --var ros_root=/opt/ros/jazzy
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ws=${1:-$HOME/hangar-baselines-ros2}
command -v colcon > /dev/null || { echo "ros2-build.sh: no colcon: source ROS 2 first (. /opt/ros/jazzy/setup.sh)" >&2; exit 1; }
mkdir -p "$ws"
cd "$ws"
colcon build --base-paths "$here/bench/pairs/ros2_pair" \
	--cmake-args -DCMAKE_BUILD_TYPE=Release -DPAIR_INCLUDE_DIR="$here/bench/pairs"
echo "ros2-build.sh: $ws/install/ros2_pair/lib/ros2_pair/ros2_pair_node"
EOF
chmod +x "$stage/ros2-build.sh"

{
	echo "hangar-baselines 1"
	echo "version $version"
	echo "target $arch $target${mcpu:+ $mcpu}"
	echo "commit $commit"
	echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ) zig $(zig version) $(rustc --version)"
	echo "flags $cc $pair_cflags -static -s"
	echo "binaries $bins"
	echo "hangar_pair null_pair unix_pair hangar_graph unix_graph: this tree, bench/pairs (GPL-3.0-or-later; lib/hg LGPL-3.0-or-later)"
	echo "iceoryx2_pair iceoryx2_graph: this tree's bench/pairs, linked with iceoryx2 $IOX2_VERSION's C binding (Apache-2.0 OR MIT)"
	echo "iceoryx2-source $IOX2_URL sha256 $IOX2_SHA256"
	echo "$ros2_line"
	echo "files"
	(cd "$stage" && find . -type f ! -name MANIFEST | sort | while read -r f; do
		printf '%s  %s\n' "$(sha256sum < "$f" | cut -d' ' -f1)" "${f#./}"
	done)
} > "$stage/MANIFEST"

tarball=$out/$name.tar.zst
tar -C "$work/stage" --sort=name --owner=0 --group=0 --numeric-owner -cf - "$name" | zstd -q -19 -T0 -f -o "$tarball"
for b in $bins; do printf '  %-16s %s\n' "$b" "$(file -b "$stage/bin/$b" | cut -d, -f1-2,4)"; done
echo "tarball: $tarball ($(du -h "$tarball" | cut -f1))"
