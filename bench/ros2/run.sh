#!/usr/bin/env bash
# bench/ros2/run.sh: hangar against a ROS 2 talker/listener pair, on this machine.
#
#   bench/ros2/run.sh [--work DIR] [--out DIR] [--hg-bench PATH] [--jobs N]
#                     [--build-only | --no-build] [--no-colcon]
#                     [--docker] [--image NAME] [--docker-cpus LIST]
#                     [what matrix.py takes: --quick, --reps N, --seconds N, --idle-seconds N,
#                      --cpus W,R, --configs a,b, --label TEXT, --max-temp C, ...]
#
# Builds bench/ros2/hangar_ros2_bench against the ROS 2 that is installed (colcon if
# there is one, cmake alone if not or with --no-colcon) into --work (~/hangar-ros2-bench unless given),
# then runs matrix.py: hg_bench and every ROS 2 middleware configuration found, the
# same sizes and rates on the same two CPUs, and writes results.json and results.md
# into --out (<work>/results/<UTC time>). README.md beside this file has the rest.
#
# It needs no root and installs nothing: what is missing is named in one line with
# the command that would add it, and left alone. It writes under --work and --out
# only, and to /dev/shm while it runs.
#
# hangar's side is --hg-bench (the release's static bin/hg_bench is the one the paper
# measures); without it, hg_bench is built here from this checkout with cc -O2.
#
# --docker runs all of the above inside a container of --image (ros:jazzy-ros-base;
# bench/ros2/Dockerfile adds Cyclone DDS and Zenoh) for a machine without ROS 2: the
# checkout mounted read-only, --work read-write, the container removed afterwards.
# --docker-cpus LIST keeps it to those CPUs (docker's --cpuset-cpus).
set -eu

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
say() { echo "bench-ros2: $*"; }
die() { echo "bench-ros2: $*" >&2; exit 1; }
usage() { sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

work=${HANGAR_ROS2_WORK:-$HOME/hangar-ros2-bench}
out=
hg_bench=
jobs=2
build=yes
run=yes
docker=no
colcon=yes
image=ros:jazzy-ros-base
docker_cpus=
commit=${HANGAR_COMMIT:-}
hg_commit=${HANGAR_HG_COMMIT:-}
pass=()
while [ $# -gt 0 ]; do
    case "$1" in
    -h|--help) usage 0 ;;
    --work) [ $# -ge 2 ] || usage 2; work=$2; shift ;;
    --out) [ $# -ge 2 ] || usage 2; out=$2; shift ;;
    --hg-bench) [ $# -ge 2 ] || usage 2; hg_bench=$2; shift ;;
    --jobs) [ $# -ge 2 ] || usage 2; jobs=$2; shift ;;
    --image) [ $# -ge 2 ] || usage 2; image=$2; shift ;;
    --docker-cpus) [ $# -ge 2 ] || usage 2; docker_cpus=$2; shift ;;
    --build-only) run=no ;;
    --no-build) build=no ;;
    --docker) docker=yes ;;
    --no-colcon) colcon=no ;;
    *) pass+=("$1") ;;
    esac
    shift
done

if [ -z "$commit" ]; then
    commit=$(git -C "$repo" describe --always --dirty 2>/dev/null || echo unknown)
fi
if [ -z "$hg_commit" ]; then
    hg_commit=$(git -C "$repo" log -1 --format=%h -- lib/hg 2>/dev/null || echo unknown)
fi
mkdir -p "$work"
work=$(cd "$work" && pwd)
stamp=$(date -u +%Y%m%dT%H%M%SZ)

# ---- in a container ------------------------------------------------------------------
if [ "$docker" = yes ]; then
    command -v docker > /dev/null 2>&1 || die "docker is not installed"
    docker image inspect "$image" > /dev/null 2>&1 ||
        die "no image $image here: docker pull $image (or docker build -t <name> $here, then --image <name>)"
    args=(--work /work --jobs "$jobs")
    [ "$build" = yes ] || args+=(--no-build)
    [ "$run" = yes ] || args+=(--build-only)
    [ "$colcon" = yes ] || args+=(--no-colcon)
    mounts=(-v "$repo:/src:ro" -v "$work:/work")
    if [ -n "$hg_bench" ]; then
        [ -x "$hg_bench" ] || die "--hg-bench $hg_bench: not an executable"
        hg_bench=$(cd "$(dirname "$hg_bench")" && pwd)/$(basename "$hg_bench")
        mounts+=(-v "$hg_bench:/hg/hg_bench:ro")
        args+=(--hg-bench /hg/hg_bench)
    fi
    if [ -n "$out" ]; then
        mkdir -p "$out"
        out=$(cd "$out" && pwd)
        mounts+=(-v "$out:/out")
        args+=(--out /out)
    else
        args+=(--out "/work/results/$stamp")
    fi
    limits=(--shm-size 2g)
    [ -z "$docker_cpus" ] || limits+=(--cpuset-cpus "$docker_cpus")
    name=hangar-ros2-bench-$$
    trap 'docker rm -f "$name" > /dev/null 2>&1 || true' EXIT INT TERM
    say "in a container of $image (removed afterwards); work directory $work"
    # The invoking user inside, so that what lands in --work is theirs.
    docker run --rm --name "$name" --user "$(id -u):$(id -g)" -e HOME=/work \
        -e HANGAR_COMMIT="$commit" -e HANGAR_HG_COMMIT="$hg_commit" \
        "${limits[@]}" "${mounts[@]}" "$image" \
        /src/bench/ros2/run.sh "${args[@]}" ${pass[@]+"${pass[@]}"}
    exit $?
fi

# ---- ROS 2 ---------------------------------------------------------------------------
if [ -z "${ROS_DISTRO:-}" ]; then
    for d in /opt/ros/jazzy /opt/ros/*; do
        if [ -f "$d/setup.bash" ]; then
            set +u
            # shellcheck disable=SC1091
            . "$d/setup.bash"
            set -u
            break
        fi
    done
fi
[ -n "${ROS_DISTRO:-}" ] ||
    die "no ROS 2 here: source its setup.bash first (source /opt/ros/jazzy/setup.bash), or use --docker"
command -v python3 > /dev/null 2>&1 || die "python3 is not installed"

src=$here/hangar_ros2_bench
prefix=$work/install/hangar_ros2_bench
if [ "$build" = yes ]; then
    for tool in cmake c++; do
        command -v "$tool" > /dev/null 2>&1 ||
            die "$tool is not installed (sudo apt install build-essential cmake); nothing was changed"
    done
    say "building $src with ROS 2 $ROS_DISTRO into $work"
    if [ "$colcon" = yes ] && command -v colcon > /dev/null 2>&1; then
        (cd "$work" && MAKEFLAGS="-j$jobs" colcon --log-base "$work/log" build \
            --base-paths "$src" --build-base "$work/build" --install-base "$work/install" \
            --event-handlers console_cohesion+ --cmake-args -DCMAKE_BUILD_TYPE=Release) > "$work/build.log" 2>&1 ||
            { tail -40 "$work/build.log" >&2; die "the build failed: $work/build.log"; }
    else
        say "without colcon: cmake alone (the same package, the same prefix)"
        { cmake -S "$src" -B "$work/build/hangar_ros2_bench" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_TESTING=OFF &&
          cmake --build "$work/build/hangar_ros2_bench" -j "$jobs" &&
          cmake --install "$work/build/hangar_ros2_bench"; } > "$work/build.log" 2>&1 ||
            { tail -40 "$work/build.log" >&2; die "the build failed: $work/build.log"; }
    fi
fi
[ -x "$prefix/lib/hangar_ros2_bench/hgb_talker" ] ||
    die "no $prefix/lib/hangar_ros2_bench/hgb_talker: build first (run without --no-build)"
# The package's own environment, without its setup files (cmake alone writes none).
export AMENT_PREFIX_PATH="$prefix${AMENT_PREFIX_PATH:+:$AMENT_PREFIX_PATH}"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ---- hangar's side -------------------------------------------------------------------
if [ -n "$hg_bench" ]; then
    [ -x "$hg_bench" ] || die "--hg-bench $hg_bench: not an executable"
elif [ "$build" = yes ] && command -v make > /dev/null 2>&1 && command -v cc > /dev/null 2>&1; then
    say "building hg_bench from $repo (cc -O2); the release's static one is the paper's: --hg-bench"
    make -f "$repo/bench/bench.mk" BUILDDIR="$work/hg" CC=cc CFLAGS=-O2 HG_COMMIT="$hg_commit" bench \
        > "$work/hg-build.log" 2>&1 || { tail -20 "$work/hg-build.log" >&2; die "hg_bench did not build: $work/hg-build.log"; }
    hg_bench=$work/hg/hg_bench
elif [ -x "$work/hg/hg_bench" ]; then
    hg_bench=$work/hg/hg_bench
fi

[ "$run" = yes ] || { say "built: $prefix${hg_bench:+ and $hg_bench}"; exit 0; }

[ -n "$out" ] || out=$work/results/$stamp
mkdir -p "$out"
exec python3 "$here/matrix.py" --prefix "$prefix" --out "$out" --commit "$commit" \
    ${hg_bench:+--hg-bench "$hg_bench"} ${pass[@]+"${pass[@]}"}
