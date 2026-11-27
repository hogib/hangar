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
#                 to the line); restart_node and a static hangard for
#                 bench/pairs/restart.py; and lcm_pair and lcm_graph, which are not
#                 static (below)
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
# on. It is cross-compiled in the build container (bench/pairs/container: the amd64
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
#   LCM 1.5.3 (LGPL-2.1): its library, static, with cmake in the build container
#     (bench/pairs/container); the two LCM nodes link it and the machine's own GLib, so
#     they are not static programs: they need an Ubuntu 24.04's libglib2.0-0, which
#     the Pi has
#
# Needs zig, make, cargo with the target's rust-std (rustup target add
# aarch64-unknown-linux-musl; for --host x86_64-unknown-linux-musl), curl, tar, zstd,
# sha256sum; docker for the LCM nodes and the ROS 2 node. Nothing is installed on this
# machine by this script; it builds one docker image (hangar-baselines-build:noble)
# and pulls ros:jazzy-ros-base for it.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

IOX2_VERSION=0.10.1
IOX2_URL=https://github.com/eclipse-iceoryx/iceoryx2/archive/refs/tags/v$IOX2_VERSION.tar.gz
IOX2_SHA256=f6ad9608f61b93e7578645ef738c3b56ea980761f8765ee47aa49423cb12d763

LCM_VERSION=1.5.3
LCM_URL=https://github.com/lcm-proj/lcm/archive/refs/tags/v$LCM_VERSION.tar.gz
LCM_SHA256=f7e693d50e229e37de00387ac8ab9228e16559dd57bddd155c4b5ca1c233834e

host=no ros2=yes out= work= image=hangar-ros2-bench:jazzy reps=1 cmd=build
build_image=hangar-baselines-build:noble

# A pinned source: fetched once into DIR/src.tar.gz, checked, unpacked into DIR/src.
fetch_source() { # dir url sha256 name
	mkdir -p "$1"
	if [ ! -f "$1/src.tar.gz" ] || [ "$(sha256sum < "$1/src.tar.gz" | cut -d' ' -f1)" != "$3" ]; then
		curl -fsSL --retry 3 -o "$1/src.tar.gz.part" "$2"
		local got; got=$(sha256sum < "$1/src.tar.gz.part" | cut -d' ' -f1)
		[ "$got" = "$3" ] || { echo "baselines.sh: $4: SHA-256 $got, expected $3" >&2; exit 1; }
		mv "$1/src.tar.gz.part" "$1/src.tar.gz"
		rm -rf "$1/src"
	fi
	if [ ! -d "$1/src" ]; then
		mkdir -p "$1/src"
		tar -xzf "$1/src.tar.gz" -C "$1/src" --strip-components=1
	fi
}
[ "${1:-}" = ros2-smoke ] && { cmd=ros2-smoke; shift; }
while [ $# -gt 0 ]; do
	case $1 in
	--host) host=yes ;;
	--no-ros2) ros2=no ;;
	--out) shift; out=$1 ;;
	--work) shift; work=$1 ;;
	--image) shift; image=$1 ;;
	--reps) shift; reps=$1 ;;
	-h|--help) sed -n '3,58p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
			--var ros_root=/opt/ros/jazzy --out /work/smoke --no-preflight
		# The restart row, for the one supervisor a container has that make check does
		# not prove: ros2 launch. (No systemd in the container, and hangard cannot start
		# a module there: docker refuses clone3 by default. make check-restart is
		# hangard on the machine itself.)
		python3 -I /b/bench/pairs/restart.py --bin /b/bin --msg /b/msg --out /work/restart-smoke --smoke \
			--only ros2-launch'
	echo "baselines.sh: $ws/smoke/report.md and $ws/restart-smoke/restart.md (a proof of the harness, not a measurement)"
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
	"$builddir/restart_node" "$builddir/hangard" > "$work/make.log" 2>&1 ||
	{ tail -20 "$work/make.log" >&2; echo "baselines.sh: make failed ($work/make.log)" >&2; exit 1; }
# The flags make gave them, for the pairs that make does not build.
pair_cflags=$(make -C "$ROOT" -s --no-print-directory BUILD=release BUILDDIR="$builddir" CC="$cc" \
	--eval='print-pair-cflags: ; @echo $(HG_CFLAGS) $(CFLAGS)' print-pair-cflags)

# iceoryx2: the pinned source, its C binding as a static library, then the pair.
iox=$work/iceoryx2
fetch_source "$iox" "$IOX2_URL" "$IOX2_SHA256" "iceoryx2 $IOX2_VERSION"
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

# What cannot be a static musl program is built in a container (bench/pairs/container): an
# Ubuntu 24.04 like the Pi's, with the aarch64 cross compiler. Without docker those
# nodes are left out, and said.
container=no
if command -v docker > /dev/null 2>&1; then
	docker image inspect "$build_image" > /dev/null 2>&1 ||
		docker build -q -t "$build_image" "$ROOT/bench/pairs/container" > "$work/build-image.log" 2>&1 ||
		{ tail -5 "$work/build-image.log" >&2; echo "baselines.sh: the build image did not build ($work/build-image.log)" >&2; exit 1; }
	container=yes
else
	echo "baselines.sh: NOTE no docker: the LCM nodes and the ROS 2 node are not built" >&2
fi
extra_bins=
lcm_line="not-built lcm_pair lcm_graph: no docker on the machine this archive was made on"
if [ "$container" = yes ]; then
	# LCM: its library from the pinned source, linked into the two nodes; GLib is the
	# machine's own.
	lcm=$work/lcm
	fetch_source "$lcm" "$LCM_URL" "$LCM_SHA256" "LCM $LCM_VERSION"
	mkdir -p "$lcm/out"
	docker run --rm --user "$(id -u):$(id -g)" -e HOME=/out -v "$ROOT:/src:ro" -v "$lcm/src:/lcm:ro" -v "$lcm/out:/out" \
		"$build_image" sh /src/bench/pairs/container/lcm.sh "$arch" > "$lcm/build.log" 2>&1 ||
		{ tail -20 "$lcm/build.log" >&2; echo "baselines.sh: LCM did not build ($lcm/build.log)" >&2; exit 1; }
	cp "$lcm/out/bin/lcm_pair" "$lcm/out/bin/lcm_graph" "$builddir/"
	extra_bins="$extra_bins lcm_pair lcm_graph"
	lcm_line="lcm_pair lcm_graph: this tree's bench/pairs, linked with LCM $LCM_VERSION (LGPL-2.1), static; GLib and the C library shared, the machine's own (Ubuntu 24.04's)"
fi

# The ROS 2 node, cross-compiled against an arm64 ROS 2 Jazzy as a sysroot.
ros2_line="not-built ros2_pair_node: ros2-build.sh builds it on the machine it runs on, against its own ROS 2 Jazzy"
ros2_ws=
cross_image=$build_image
if [ "$host" = no ] && [ "$ros2" = yes ]; then
	if [ "$container" = no ]; then
		echo "baselines.sh: NOTE no docker: the ROS 2 node is not prebuilt (ros2-build.sh on the Pi)" >&2
	else
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
bins="hangar_pair null_pair unix_pair iceoryx2_pair hangar_graph unix_graph iceoryx2_graph restart_node hangard$extra_bins"
for b in $bins; do cp "$builddir/$b" "$stage/bin/"; done
if [ "$container" = yes ]; then
	mkdir -p "$stage/licenses/lcm"
	cp "$work/lcm/src/COPYING" "$stage/licenses/lcm/"
fi
# The driver and what it reads, the ROS 2 node's source with them; no caches.
(cd "$ROOT" && tar -cf - --exclude=__pycache__ --exclude='*.o' bench/pairs tools/rov/preflight.py msg/depth.msg) | tar -xf - -C "$stage"
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
	echo "hangar_pair null_pair unix_pair hangar_graph unix_graph restart_node: this tree, bench/pairs (GPL-3.0-or-later; lib/hg LGPL-3.0-or-later)"
	echo "hangard: this tree's manager, for bench/pairs/restart.py alone: the same source as the release's, built here (GPL-3.0-or-later)"
	echo "iceoryx2_pair iceoryx2_graph: this tree's bench/pairs, linked with iceoryx2 $IOX2_VERSION's C binding (Apache-2.0 OR MIT)"
	echo "iceoryx2-source $IOX2_URL sha256 $IOX2_SHA256"
	echo "$lcm_line"
	echo "lcm-source $LCM_URL sha256 $LCM_SHA256"
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
