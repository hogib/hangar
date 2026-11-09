#!/usr/bin/env bash
# tools/rov/rov.sh (in a release: share/hangar/rov.sh): hangar on the lab's BlueROV2,
# both roles on the vehicle's Pi (tools/rov/README.md). ArduSub runs on a Pixhawk on
# USB; MAVROS relays it to the vehicle's mavlink module over UDP; the topside role
# sends to QGroundControl on the laptop across the tether. Until the bridge the two
# managers share the Pi and read each other's topics through the same-host shim.
#
#   rov.sh up                 the relay, then both managers (roots laid out once)
#   rov.sh down               both managers, then the relay
#   rov.sh status             both managers' status, the relay's
#   rov.sh reset              down, then remove both roots (the next up lays them out)
#   rov.sh relay up|down      MAVROS alone
#   rov.sh params save [FILE] every Pixhawk parameter to FILE (default under $HANGAR_ROV_STATE)
#   rov.sh params diff FILE   the parameters that differ from FILE now
#   rov.sh params restore FILE  set back the ones that differ, then diff again (disarmed only)
#
# Environment: ROV_IP (192.168.2.10) and GCS_IP (192.168.2.1), the Pi's and the
# laptop's addresses on the tether; FCU (/dev/ttyACM0:115200), the Pixhawk;
# HANGAR_SESSIONS (~/hangar-sessions), where the recorder writes; HANGAR_ROV_STATE
# (~/hangar-rov), the relay's log and the parameter files; HANGAR_BIN, the binaries;
# ROS_SETUP (/opt/ros/jazzy/setup.bash); RELAY=0 starts no relay (a test without a Pixhawk).
set -euo pipefail

here=$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")
if [ -d "$here/profiles/rov-vehicle" ]; then          # a release: share/hangar/
    profiles=$here/profiles; msgdir=$here/msg
    bin=${HANGAR_BIN:-$(realpath -m -- "$here/../../bin")}
else                                                    # the repository: tools/rov/
    repo=$(realpath -- "$here/../..")
    profiles=$repo/profiles; msgdir=$repo/msg
    bin=${HANGAR_BIN:-$repo/builddir}
fi
RV=/dev/shm/hangar-vehicle
RT=/dev/shm/hangar-topside
ROV_IP=${ROV_IP:-192.168.2.10}
GCS_IP=${GCS_IP:-192.168.2.1}
FCU=${FCU:-/dev/ttyACM0:115200}
SESSIONS=${HANGAR_SESSIONS:-$HOME/hangar-sessions}
STATE=${HANGAR_ROV_STATE:-$HOME/hangar-rov}
ROS_SETUP=${ROS_SETUP:-/opt/ros/jazzy/setup.bash}
RELAY=${RELAY:-1}

usage() { sed -n '2,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
say() { echo "rov: $*"; }
die() { echo "rov: $*" >&2; exit 1; }

# ---- the roots ------------------------------------------------------------------

# A profile's files with the installed paths rewritten: /usr/lib/hangar -> the
# binaries, /etc/hangar and /var/lib/hangar -> the root's, the recorder's sessions
# -> $SESSIONS, the tether addresses -> ROV_IP and GCS_IP.
lay_out() { # root profile
    local root=$1 prof=$2 f b
    mkdir -p "$root/etc/hangar/modules" "$root/usr/share/hangar/msg" "$root/var/lib/hangar/safety" \
        "$root/var/lib/hangar/mavlink" "$root/run/hangar"
    cp "$msgdir"/*.msg "$root/usr/share/hangar/msg/"
    for f in "$prof"/*.module; do
        sed -e "s|/usr/lib/hangar/|$bin/|g" -e "s|/etc/hangar/|$root/etc/hangar/|g" "$f" \
            > "$root/etc/hangar/modules/$(basename "$f")"
    done
    for f in "$prof"/*.conf "$prof"/*.params; do
        [ -e "$f" ] || continue
        b=$(basename "$f")
        case $b in
            hangard.conf) cp "$f" "$root/etc/hangar/hangard.conf" ;;
            safety.params) cp "$f" "$root/var/lib/hangar/safety/params" ;;
            *) sed -e "s|/etc/hangar/|$root/etc/hangar/|g" \
                   -e "s|/var/lib/hangar/recorder/sessions|$SESSIONS|g" \
                   -e "s|/var/lib/hangar/|$root/var/lib/hangar/|g" \
                   -e "s|^Bind=192\.168\.2\.10:|Bind=$ROV_IP:|" \
                   -e "s|^GCS=192\.168\.2\.1:|GCS=$GCS_IP:|" "$f" > "$root/etc/hangar/$b" ;;
        esac
    done
}

running() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }

start_manager() { # root peer-root
    local root=$1 peer=$2
    if running "$root/hangard.pid"; then
        say "a manager already runs under $root (pid $(cat "$root/hangard.pid"))"
        return 0
    fi
    HANGAR_ROOT=$root HANGAR_PEER_TOPIC_DIR=$peer/dev/shm/hangar \
        setsid nohup "$bin/hangard" >> "$root/hangard.log" 2>&1 < /dev/null &
    echo $! > "$root/hangard.pid"
}

stop_manager() { # root
    local root=$1 pid
    running "$root/hangard.pid" || { rm -f "$root/hangard.pid"; return 0; }
    pid=$(cat "$root/hangard.pid")
    kill -TERM "$pid"
    for _ in $(seq 1 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill -0 "$pid" 2>/dev/null && { say "manager $pid did not stop in 10 s: SIGKILL"; kill -KILL "$pid"; }
    rm -f "$root/hangard.pid"
}

# ---- the relay ------------------------------------------------------------------

# MAVROS, run in its own process group (setsid) so that stopping it is a signal to
# the group, never a pattern match. Its own MAVLink identity is system 2: the vehicle
# module drops other components of ArduSub's system since 3a1dfeb, but a release
# before that took MAVROS's system-1 heartbeat for ArduSub's.
relay_up() {
    [ "$RELAY" = 1 ] || { say "RELAY=0: no relay"; return 0; }
    mkdir -p "$STATE"
    if running "$STATE/relay.pid"; then say "the relay already runs (pid $(cat "$STATE/relay.pid"))"; return 0; fi
    [ -f "$ROS_SETUP" ] || die "no ROS at $ROS_SETUP (MAVROS is the relay; set ROS_SETUP)"
    [ -e "${FCU%%:*}" ] || die "no Pixhawk at ${FCU%%:*}"
    setsid nohup bash -c "source '$ROS_SETUP' && exec ros2 run mavros mavros_node --ros-args \
        -p fcu_url:=$FCU -p gcs_url:=udp://127.0.0.1:14555@127.0.0.1:14560 \
        -p system_id:=2 -p component_id:=191 -p tgt_system:=1 -p tgt_component:=1" \
        >> "$STATE/relay.log" 2>&1 < /dev/null &
    echo $! > "$STATE/relay.pid"
    say "relay: MAVROS $FCU -> 127.0.0.1:14560 (pid $!, log $STATE/relay.log)"
}

relay_down() {
    running "$STATE/relay.pid" || { rm -f "$STATE/relay.pid"; return 0; }
    local pid; pid=$(cat "$STATE/relay.pid")
    kill -INT -- "-$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill -0 "$pid" 2>/dev/null && kill -KILL -- "-$pid" 2>/dev/null || true
    rm -f "$STATE/relay.pid"
    say "relay stopped"
}

# ---- the Pixhawk's parameters, through MAVROS ---------------------------------

ros() { set +u; source "$ROS_SETUP"; set -u; }

need_relay() { running "$STATE/relay.pid" || die "the relay is not running: rov.sh relay up"; }

# NAME: value, one per line, sorted; the node's own ROS parameters left out.
flatten() { grep -E '^ {4}[A-Z][A-Z0-9_]*: ' "$1" | sed 's/^ *//' | sort; }

params_dump() { # file
    ros
    timeout 120 ros2 service call /mavros/param/pull mavros_msgs/srv/ParamPull "{force_pull: true}" \
        | grep -q 'success=True' || die "MAVROS could not pull the parameters"
    timeout 60 ros2 param dump /mavros/param > "$1"
}

params_save() {
    need_relay
    mkdir -p "$STATE"
    local f=${1:-$STATE/pixhawk-$(date +%Y%m%d-%H%M%S).yaml}
    params_dump "$f"
    say "$(flatten "$f" | wc -l) parameters saved to $f"
}

params_diff() { # file; exit status 1 when something differs
    need_relay
    [ -f "$1" ] || die "no file $1"
    local now; now=$(mktemp)
    params_dump "$now"
    if diff <(flatten "$1") <(flatten "$now") > "$now.diff"; then
        say "every parameter as in $1"; rm -f "$now" "$now.diff"; return 0
    fi
    grep '^<' "$now.diff" | sed 's/^< /  was  /'
    grep '^>' "$now.diff" | sed 's/^> /  now  /'
    rm -f "$now" "$now.diff"
    return 1
}

params_restore() { # file
    need_relay
    [ -f "$1" ] || die "no file $1"
    ros
    timeout 10 ros2 topic echo --once /mavros/state 2>/dev/null | grep -q 'armed: false' \
        || die "the Pixhawk is armed, or MAVROS cannot say: restore only while disarmed"
    local now set; now=$(mktemp); set=$(mktemp)
    params_dump "$now"
    { echo "/mavros/param:"; echo "  ros__parameters:"
      comm -23 <(flatten "$1") <(flatten "$now") | sed 's/^/    /'; } > "$set"
    local n; n=$(($(wc -l < "$set") - 2))
    if [ "$n" -eq 0 ]; then say "nothing to restore"; rm -f "$now" "$set"; return 0; fi
    say "restoring $n parameters:"; tail -n +3 "$set"
    timeout 120 ros2 param load /mavros/param "$set"
    rm -f "$now" "$set"
    sleep 2
    params_diff "$1"
}

# ---- commands -------------------------------------------------------------------

cmd_up() {
    for b in hangard hangar hangar-safety hangar-mux hangar-mavlink recorder dawn-patrold; do
        [ -x "$bin/$b" ] || die "missing $bin/$b (set HANGAR_BIN)"
    done
    relay_up
    [ -d "$RV/etc/hangar" ] || lay_out "$RV" "$profiles/rov-vehicle"
    [ -d "$RT/etc/hangar" ] || lay_out "$RT" "$profiles/rov-topside"
    start_manager "$RV" "$RT"
    start_manager "$RT" "$RV"
    sleep 1
    cat <<MSG
rov: hangar is up on this Pi (both roles; the shim stands in for the bridge)
  vehicle  $RV  safety, mux, mavlink, recorder, console on http://127.0.0.1:8080
  topside  $RT  topside_mavlink -> QGroundControl at $GCS_IP:14550, console on :8081
  logs     <root>/hangard.log, <root>/run/hangar/log/<module>.log, $STATE/relay.log
  sessions $SESSIONS (recorded while armed)
Before the first arm (writes ArduSub's parameters; save them first):
  rov.sh params save
  HANGAR_ROOT=$RV $bin/hangar ardusub-setup
  HANGAR_ROOT=$RV $bin/hangar check
MSG
}

cmd_down() {
    stop_manager "$RT"
    stop_manager "$RV"
    relay_down
    say "down"
}

cmd_status() {
    for root in "$RV" "$RT"; do
        echo "== $root"
        if running "$root/hangard.pid"; then HANGAR_ROOT=$root "$bin/hangar" status || true
        else echo "no manager"; fi
    done
    echo "== relay"
    if running "$STATE/relay.pid"; then echo "MAVROS pid $(cat "$STATE/relay.pid")"; else echo "not running"; fi
}

case "${1:-}" in
    up) cmd_up ;;
    down) cmd_down ;;
    status) cmd_status ;;
    reset) cmd_down; rm -rf "$RV" "$RT"; say "roots removed" ;;
    relay)
        case "${2:-}" in up) relay_up ;; down) relay_down ;; *) usage 2 ;; esac ;;
    params)
        case "${2:-}" in
            save) params_save "${3:-}" ;;
            diff) [ -n "${3:-}" ] || usage 2; params_diff "$3" ;;
            restore) [ -n "${3:-}" ] || usage 2; params_restore "$3" ;;
            *) usage 2 ;;
        esac ;;
    -h|--help|help|'') usage 0 ;;
    *) echo "rov: unknown command $1" >&2; usage 2 ;;
esac
