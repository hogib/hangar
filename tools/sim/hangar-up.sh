#!/usr/bin/env bash
# tools/sim/hangar-up.sh: hangar in the loop of the drivable demo (modules/PHASE1.md,
# tools/sim/SETUP.md "Where hangar goes"). Two managers on the host, one per role,
# under two roots on tmpfs, joined by the same-host shim (HANGAR_PEER_TOPIC_DIR:
# each root reads the other's topic files directly, until the bridge exists):
#
#   /dev/shm/hangar-vehicle   Role=vehicle: safety, mux, mavlink (SITL on 127.0.0.1:14560),
#                             Dawn Patrol on http://127.0.0.1:8080
#   /dev/shm/hangar-topside   Role=topside: topside_mavlink (QGroundControl on 14550),
#                             Dawn Patrol on http://127.0.0.1:8081
#
#   hangar-up.sh up       build nothing; lay out both roots from profiles/sim-*,
#                         start both managers
#   hangar-up.sh status   `hangar status` of both
#   hangar-up.sh down     stop both managers, remove the roots (--keep keeps them)
#
# The whole demo: HANGAR_SIM=1 tools/sim/run.sh up; tools/sim/hangar-up.sh up;
# qgroundcontrol; then in QGC arm and drive. Environment: BUILDDIR (default
# builddir), HANGAR_VEHICLE_ROOT, HANGAR_TOPSIDE_ROOT.
set -euo pipefail

here=$(command dirname -- "$(command realpath -- "${BASH_SOURCE[0]}")")
repo=$(command realpath -- "$here/../..")
builddir=$(command realpath -m -- "${BUILDDIR:-$repo/builddir}")
root_v=${HANGAR_VEHICLE_ROOT:-/dev/shm/hangar-vehicle}
root_t=${HANGAR_TOPSIDE_ROOT:-/dev/shm/hangar-topside}

usage() {
    command sed -n '2,22p' "${BASH_SOURCE[0]}" | command sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

need() {
    for b in "$@"; do
        [ -x "$builddir/$b" ] || { echo "missing $builddir/$b: run make (or set BUILDDIR)" >&2; exit 1; }
    done
}

# Lays out one root from a profile: the manifests and configuration files with
# /usr/lib/hangar -> the build directory and /etc/hangar -> the root's etc/hangar.
lay_out() { # root profile-dir
    local root=$1 prof=$2 f
    command mkdir -p "$root/etc/hangar/modules" "$root/usr/share/hangar/msg" "$root/var/lib/hangar/safety" \
        "$root/var/lib/hangar/mavlink" "$root/run/hangar"
    command cp "$repo"/msg/*.msg "$root/usr/share/hangar/msg/"
    for f in "$prof"/*.module; do
        command sed -e "s|/usr/lib/hangar/|$builddir/|g" -e "s|/etc/hangar/|$root/etc/hangar/|g" "$f" \
            > "$root/etc/hangar/modules/$(command basename "$f")"
    done
    for f in "$prof"/*.conf "$prof"/*.params; do
        [ -e "$f" ] || continue
        case "$(command basename "$f")" in
            hangard.conf) command cp "$f" "$root/etc/hangar/hangard.conf" ;;
            safety.params) command cp "$f" "$root/var/lib/hangar/safety/params" ;;
            *) command sed -e "s|/etc/hangar/|$root/etc/hangar/|g" -e "s|/dev/shm/hangar-vehicle|$root_v|g" "$f" \
                   > "$root/etc/hangar/$(command basename "$f")" ;;
        esac
    done
}

start_manager() { # root peer-root
    local root=$1 peer=$2
    if [ -f "$root/hangard.pid" ] && kill -0 "$(command cat "$root/hangard.pid")" 2>/dev/null; then
        echo "a manager already runs under $root (pid $(command cat "$root/hangard.pid"))"
        return 0
    fi
    HANGAR_ROOT=$root HANGAR_PEER_TOPIC_DIR=$peer/dev/shm/hangar \
        nohup "$builddir/hangard" >> "$root/hangard.log" 2>&1 &
    echo $! > "$root/hangard.pid"
}

cmd_up() {
    need hangard hangar hangar-safety hangar-mux hangar-mavlink
    [ -x "$builddir/dawn-patrold" ] || echo "note: $builddir/dawn-patrold is not built; Dawn Patrol will be refused by the manager" >&2
    lay_out "$root_v" "$repo/profiles/sim-vehicle"
    lay_out "$root_t" "$repo/profiles/sim-topside"
    start_manager "$root_v" "$root_t"
    start_manager "$root_t" "$root_v"
    sleep 0.5
    cat <<MSG
hangar is up, two managers on this host (the "tether" is the same-host shim, until the bridge):
  vehicle  root $root_v  (safety, mux, mavlink, console on http://127.0.0.1:8080)
  topside  root $root_t  (topside_mavlink, console on http://127.0.0.1:8081)
  logs     $root_v/hangard.log, $root_t/hangard.log, <root>/run/hangar/log/<module>.log

The vehicle's mavlink module waits for SITL on 127.0.0.1:14560:
  HANGAR_SIM=1 $here/run.sh up          (MAVProxy --out 127.0.0.1:14560, no 14550)
QGroundControl connects to the topside's module on 14550 as it would to a vehicle:
  qgroundcontrol                          (Manual, Disarmed; arm there, drive with the sticks)
Before the first arm, once per SITL start (SITL starts with default parameters):
  HANGAR_ROOT=$root_v $builddir/hangar ardusub-setup
  HANGAR_ROOT=$root_v $builddir/hangar check
Look inside:
  HANGAR_ROOT=$root_v $builddir/hangar status
  HANGAR_ROOT=$root_v $builddir/hangar echo safety.state
  HANGAR_ROOT=$root_v $builddir/hangar echo ardusub.state
  HANGAR_ROOT=$root_t $builddir/hangar echo pilot.wrench --rate
MSG
}

cmd_status() {
    need hangar
    for root in "$root_v" "$root_t"; do
        echo "== $root"
        HANGAR_ROOT=$root "$builddir/hangar" status || true
    done
}

cmd_down() {
    local keep=${1:-}
    for root in "$root_v" "$root_t"; do
        if [ -f "$root/hangard.pid" ]; then
            local pid; pid=$(command cat "$root/hangard.pid")
            if kill -0 "$pid" 2>/dev/null; then
                kill -TERM "$pid"
                for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
                kill -0 "$pid" 2>/dev/null && kill -KILL "$pid"
            fi
            command rm -f "$root/hangard.pid"
        fi
        if [ "$keep" != --keep ]; then
            command rm -rf "$root"
        fi
    done
    echo "hangar is down${keep:+ (roots kept)}"
}

case "${1:-}" in
    up)     cmd_up ;;
    status) cmd_status ;;
    down)   cmd_down "${2:-}" ;;
    -h|--help|help|'') usage 0 ;;
    *) echo "unknown command: $1" >&2; usage 2 ;;
esac
