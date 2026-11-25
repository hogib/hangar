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
#   rov.sh relay up|down|status  MAVROS alone
#   rov.sh params save [FILE] every Pixhawk parameter to FILE (default under $HANGAR_ROV_STATE)
#   rov.sh params diff FILE   the parameters that differ from FILE now
#   rov.sh params restore FILE  set back the ones that differ, then diff again (disarmed only)
#   rov.sh params via         which way the three go: hangar, mavros or none
#     Through hangar (hangar ardusub-params) whenever the vehicle's manager is up, over
#     MAVROS's UDP or Serial= alike; through MAVROS alone only when hangar is not up.
#     hangar's files are NAME VALUE TYPE lines; MAVROS's, the YAML of ros2 param
#     dump. hangar reads both; the MAVROS fallback, its own YAML only.
#   rov.sh topside up|down|status  on the laptop, with BRIDGE=1: the topside role there
#
# BRIDGE=1 (opt-in) replaces the shim with the tether bridge: `rov.sh up` on the Pi
# runs the vehicle role alone, with the bridge, and `rov.sh topside up` on the laptop
# the topside role, with its bridge (README "The topside on the laptop"). Both take
# the key from $HANGAR_ROV_STATE/bridge.key (32 bytes; up on the Pi makes one).
#
# Environment: ROV_IP (192.168.2.10) and GCS_IP (192.168.2.1), the Pi's and the
# laptop's addresses on the tether; FCU (/dev/ttyACM0:115200), the Pixhawk;
# HANGAR_SESSIONS (~/hangar-sessions), where the recorder writes; HANGAR_ROV_STATE
# (~/hangar-rov), the relay's log and the parameter files; HANGAR_BIN, the binaries;
# ROS_SETUP (/opt/ros/jazzy/setup.bash); RELAY=0 starts no relay (a test without a Pixhawk);
# ENCODER=1 lays out the encoder too (the camera's own H.264 to the laptop; README);
# ENCODER_SOURCE (uvc:/dev/video2 on the lab's camera) its Source=, set at lay-out;
# MAVLINK=serial lays out the profile's mavlink.conf.serial and mavlink.module.serial:
# the mavlink module on the Pixhawk's serial line itself ($FCU's device and rate), no
# relay, and nothing else may hold the device (modules/mavlink/README.md, "A serial line");
# BRIDGE_PORT and BRIDGE_PEER_PORT (5614 both), the bridge's own UDP port and its peer's:
# two different ones only for both roles on one machine (a rehearsal on loopback).
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
ENCODER=${ENCODER:-0}
ENCODER_SOURCE=${ENCODER_SOURCE:-}
BRIDGE=${BRIDGE:-0}
BRIDGE_PORT=${BRIDGE_PORT:-5614}
BRIDGE_PEER_PORT=${BRIDGE_PEER_PORT:-5614}
MAVLINK=${MAVLINK:-udp}
case $MAVLINK in udp|serial) ;; *) echo "rov: MAVLINK=$MAVLINK: udp or serial" >&2; exit 2 ;; esac
# MAVLINK=serial: the module holds the Pixhawk itself, so there is no relay to start.
[ "$MAVLINK" = serial ] && RELAY=0
# The address the topside's MAVLink module binds: the machine the topside role runs on.
# The Pi's by default; rov.sh topside (the role on the laptop) makes it the laptop's.
TOPSIDE_BIND_IP=$ROV_IP
# And where it finds QGroundControl: the laptop, from the Pi. With the role on the laptop
# both are the loopback address, see cmd_topside.
TOPSIDE_GCS_IP=$GCS_IP
# The page the topside console's Hellion tab frames. The profile's is the lab's own
# hellion (port 8080); tools/rov/lab.sh runs its hellion on another port and says which.
TOPSIDE_HELLION_URL=${TOPSIDE_HELLION_URL:-}

usage() { sed -n '2,39p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
say() { echo "rov: $*"; }
die() { echo "rov: $*" >&2; exit 1; }

# ---- the roots ------------------------------------------------------------------

# A profile's files with the installed paths rewritten: /usr/lib/hangar -> the
# binaries, /etc/hangar and /var/lib/hangar -> the root's, the recorder's sessions
# -> $SESSIONS, the tether addresses -> ROV_IP and GCS_IP.
conf_sed() { # root: a profile's .conf on stdin, as laid out, on stdout
    sed -e "s|/etc/hangar/|$1/etc/hangar/|g" \
        -e "s|/var/lib/hangar/recorder/sessions|$SESSIONS|g" \
        -e "s|/var/lib/hangar/|$1/var/lib/hangar/|g" \
        -e "s|^Bind=192\.168\.2\.10:|Bind=$TOPSIDE_BIND_IP:|" \
        -e "s|^GCS=192\.168\.2\.1:|GCS=$TOPSIDE_GCS_IP:|" \
        -e "s|^Peer=192\.168\.2\.1:|Peer=$GCS_IP:|"
}

lay_out() { # root profile
    local root=$1 prof=$2 f b
    mkdir -p "$root/etc/hangar/modules" "$root/usr/share/hangar/msg" "$root/var/lib/hangar/safety" \
        "$root/var/lib/hangar/mavlink" "$root/run/hangar"
    cp "$msgdir"/*.msg "$root/usr/share/hangar/msg/"
    for f in "$prof"/*.module; do
        sed -e "s|/usr/lib/hangar/|$bin/|g" -e "s|/etc/hangar/|$root/etc/hangar/|g" "$f" \
            > "$root/etc/hangar/modules/$(basename "$f")"
    done
    if [ -n "$TOPSIDE_HELLION_URL" ] && [ "$(basename "$prof")" = rov-topside ]; then
        sed -i -e "s|--hellion http://[^ ]*|--hellion $TOPSIDE_HELLION_URL|" "$root/etc/hangar/modules/console.module"
        grep -q -- "--hellion $TOPSIDE_HELLION_URL" "$root/etc/hangar/modules/console.module" ||
            die "the topside's console.module has no --hellion to set (TOPSIDE_HELLION_URL)"
    fi
    # The encoder is opt-in: its manifest is encoder.module.off in the profile.
    if [ "$ENCODER" = 1 ] && [ -e "$prof/encoder.module.off" ]; then
        sed -e "s|/usr/lib/hangar/|$bin/|g" -e "s|/etc/hangar/|$root/etc/hangar/|g" "$prof/encoder.module.off" \
            > "$root/etc/hangar/modules/encoder.module"
    fi
    for f in "$prof"/*.conf "$prof"/*.params; do
        [ -e "$f" ] || continue
        b=$(basename "$f")
        case $b in
            hangard.conf) cp "$f" "$root/etc/hangar/hangard.conf" ;;
            safety.params) cp "$f" "$root/var/lib/hangar/safety/params" ;;
            *) conf_sed "$root" < "$f" > "$root/etc/hangar/$b" ;;
        esac
    done
    # The serial line is opt-in: mavlink.conf.serial and mavlink.module.serial in the
    # place of the two beside them, the device and the rate from $FCU. The profile's
    # own files are read, never changed.
    if [ "$MAVLINK" = serial ] && [ -e "$prof/mavlink.conf.serial" ]; then
        local rate=115200
        [[ $FCU == *:* ]] && rate=${FCU##*:}
        sed -e "s|/usr/lib/hangar/|$bin/|g" -e "s|/etc/hangar/|$root/etc/hangar/|g" "$prof/mavlink.module.serial" \
            > "$root/etc/hangar/modules/mavlink.module"
        conf_sed "$root" < "$prof/mavlink.conf.serial" |
            sed -e "s|^Serial=.*|Serial=${FCU%%:*}|" -e "s|^Baud=.*|Baud=$rate|" > "$root/etc/hangar/mavlink.conf"
    fi
    # The bridge is opt-in: bridge.module.off, bridge.conf and the shared key.
    if [ "$BRIDGE" = 1 ] && [ -e "$prof/bridge.module.off" ]; then
        sed -e "s|/usr/lib/hangar/|$bin/|g" -e "s|/etc/hangar/|$root/etc/hangar/|g" "$prof/bridge.module.off" \
            > "$root/etc/hangar/modules/bridge.module"
        # Peer= names a port only when it is not this end's own (the bridge's default).
        local pport=""
        [ "$BRIDGE_PEER_PORT" = "$BRIDGE_PORT" ] || pport=:$BRIDGE_PEER_PORT
        sed -i -e "s|/usr/share/hangar/|$root/usr/share/hangar/|g" -e "s|^Port=5614$|Port=$BRIDGE_PORT|" \
               -e "s|^Peer=192\.168\.2\.1$|Peer=$GCS_IP$pport|" -e "s|^Peer=192\.168\.2\.10$|Peer=$ROV_IP$pport|" \
            "$root/etc/hangar/bridge.conf"
        install -m 600 "$STATE/bridge.key" "$root/etc/hangar/bridge.key"
        # With a bridge the tether is watched (docs/07 "Failsafe"): the profile's
        # safety.params says tether_enabled = 0 for the layout without one.
        if [ -e "$root/var/lib/hangar/safety/params" ]; then
            sed -i -e 's|^tether_enabled = 0$|tether_enabled = 1|' "$root/var/lib/hangar/safety/params"
        fi
    fi
    if [ -n "$ENCODER_SOURCE" ] && [ -e "$root/etc/hangar/encoder.conf" ]; then
        sed -i "s|^Source=.*|Source=$ENCODER_SOURCE|" "$root/etc/hangar/encoder.conf"
    fi
}

# A pid file names a running process only if that process is the one it was written
# for. $STATE is on the Pi's card: relay.pid outlives a reboot, after which its number
# is some other process's (and relay down signals the whole group of it). Beside each
# pid file, <file>.id holds the boot's id and the process's start time in clock ticks
# since boot; neither moves when the Pi's clock is set. A pid file without one (written
# by a release before this) is taken at its word, as before.
proc_id() { # pid
    local stat f
    stat=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
    read -r -a f <<< "${stat##*) }"       # after "pid (comm) ": state is f[0], starttime f[19]
    [ -n "${f[19]:-}" ] || return 1
    echo "$(cat /proc/sys/kernel/random/boot_id) ${f[19]}"
}
write_pid() { # pid file
    echo "$1" > "$2"
    proc_id "$1" > "$2.id" || rm -f "$2.id"
}
drop_pid() { rm -f "$1" "$1.id"; }
running() { # file
    local pid
    [ -f "$1" ] || return 1
    pid=$(cat "$1")
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null || return 1
    [ -f "$1.id" ] || return 0
    [ "$(cat "$1.id")" = "$(proc_id "$pid")" ]
}

start_manager() { # root [peer-root: the same-host shim]
    local root=$1 peer=${2:-}
    if running "$root/hangard.pid"; then
        say "a manager already runs under $root (pid $(cat "$root/hangard.pid"))"
        return 0
    fi
    if [ -n "$peer" ]; then
        HANGAR_ROOT=$root HANGAR_PEER_TOPIC_DIR=$peer/dev/shm/hangar \
            setsid nohup "$bin/hangard" >> "$root/hangard.log" 2>&1 < /dev/null &
    else
        HANGAR_ROOT=$root setsid nohup "$bin/hangard" >> "$root/hangard.log" 2>&1 < /dev/null &
    fi
    write_pid $! "$root/hangard.pid"
}

# BRIDGE=1: the key both bridges share; made on the Pi if absent, copied to the
# laptop by hand (README).
need_key() { # make
    [ -s "$STATE/bridge.key" ] && return 0
    [ "$1" = make ] || die "no $STATE/bridge.key: copy the Pi's (scp $ROV_IP:hangar-rov/bridge.key $STATE/)"
    mkdir -p "$STATE"
    (umask 077; head -c 32 /dev/urandom > "$STATE/bridge.key")
    say "made $STATE/bridge.key: copy it to the laptop's $STATE/"
}

# The roots must have been laid out the way this run wants them.
same_layout() { # root
    local has=0
    [ -e "$1/etc/hangar/modules/bridge.module" ] && has=1
    [ ! -d "$1/etc/hangar" ] || [ "$has" = "$BRIDGE" ] ||
        die "$1 was laid out with BRIDGE=$has: rov.sh reset (or topside reset) first"
    local link=udp
    grep -q '^Serial=' "$1/etc/hangar/mavlink.conf" 2>/dev/null && link=serial
    [ ! -e "$1/etc/hangar/mavlink.conf" ] || [ "$link" = "$MAVLINK" ] ||
        die "$1 was laid out with MAVLINK=$link: rov.sh reset first"
}

# MAVLINK=serial: the line must be there and nobody else's. The module opens it
# exclusively; a relay of an earlier up would have it get EBUSY for ever.
need_serial() {
    [ "$MAVLINK" = serial ] || return 0
    [ -e "${FCU%%:*}" ] || die "no Pixhawk at ${FCU%%:*}"
    ! running "$STATE/relay.pid" ||
        die "MAVLINK=serial, and the relay holds ${FCU%%:*} (pid $(cat "$STATE/relay.pid")): rov.sh relay down first"
}

stop_manager() { # root
    local root=$1 pid
    running "$root/hangard.pid" || { drop_pid "$root/hangard.pid"; return 0; }
    pid=$(cat "$root/hangard.pid")
    kill -TERM "$pid"
    for _ in $(seq 1 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill -0 "$pid" 2>/dev/null && { say "manager $pid did not stop in 10 s: SIGKILL"; kill -KILL "$pid"; }
    drop_pid "$root/hangard.pid"
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
    write_pid $! "$STATE/relay.pid"
    say "relay: MAVROS $FCU -> 127.0.0.1:14560 (pid $!, log $STATE/relay.log)"
}

relay_down() {
    running "$STATE/relay.pid" || { drop_pid "$STATE/relay.pid"; return 0; }
    local pid; pid=$(cat "$STATE/relay.pid")
    kill -INT -- "-$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill -0 "$pid" 2>/dev/null && kill -KILL -- "-$pid" 2>/dev/null || true
    drop_pid "$STATE/relay.pid"
    say "relay stopped"
}

# ---- the Pixhawk's parameters, through MAVROS ---------------------------------

ros() { set +u; source "$ROS_SETUP"; set -u; }

need_relay() { running "$STATE/relay.pid" || die "the relay is not running: rov.sh relay up"; }

# ---- ArduSub's parameters ----------------------------------------------------------
#
# hangar's way (hangar ardusub-params: the vehicle's mavlink module reads and writes them
# itself) whenever the vehicle's manager is up; MAVROS's only when it is not.

hg_vehicle() { HANGAR_ROOT=$RV "$bin/hangar" "$@"; }

params_via() {
    if running "$RV/hangard.pid"; then echo hangar
    elif running "$STATE/relay.pid"; then echo mavros
    else echo none; fi
}

# The module must have heard ArduSub (a minute after an up, at most): ready is every
# rated output published, the telemetry among them (lab.sh waits the same way).
wait_ardusub() {
    local t=0
    while [ "$t" -lt 60 ]; do
        hg_vehicle status 2>/dev/null | awk '$1 == "mavlink"' |
            grep -E 'ready .*ardusub (disarmed|armed|disarming)' > /dev/null && return 0
        sleep 2; t=$((t + 2))
    done
    die "the vehicle's mavlink module has not heard ArduSub in 60 s (rov.sh status)"
}

# A backup with no parameter in it restores nothing and compares equal to nothing: it
# is not kept, and the save fails.
some_params() { # file count
    [ "${2:-0}" -gt 0 ] && return 0
    rm -f "$1"
    die "the dump has no parameters in it: nothing saved (ArduSub answered with none?)"
}

no_way() { die "neither hangar (rov.sh up) nor MAVROS (rov.sh relay up) runs: no way to the Pixhawk"; }

params_save() {
    mkdir -p "$STATE"
    case $(params_via) in
        hangar)
            local f=${1:-$STATE/pixhawk-$(date +%Y%m%d-%H%M%S).params}
            wait_ardusub
            hg_vehicle ardusub-params save "$f" || die "hangar could not save the parameters"
            some_params "$f" "$(grep -cE '^[A-Z][A-Z0-9_]* ' "$f" || true)"
            say "saved through hangar to $f" ;;
        mavros) mavros_params_save "${1:-}" ;;
        *) no_way ;;
    esac
}

params_diff() { # file; exit status 1 when something differs
    [ -f "$1" ] || die "no file $1"
    case $(params_via) in
        hangar)
            wait_ardusub
            local rc=0; hg_vehicle ardusub-params diff "$1" || rc=$?
            [ "$rc" -le 1 ] || die "hangar could not compare the parameters"
            return "$rc" ;;
        mavros) mavros_params_diff "$1" ;;
        *) no_way ;;
    esac
}

params_restore() { # file
    [ -f "$1" ] || die "no file $1"
    case $(params_via) in
        hangar)
            wait_ardusub
            hg_vehicle ardusub-params restore "$1" || die "hangar could not restore the parameters"
            params_diff "$1" ;;
        mavros) mavros_params_restore "$1" ;;
        *) no_way ;;
    esac
}

# ---- the same through MAVROS, when hangar is not up ------------------------------

# NAME: value, one per line, sorted; the node's own ROS parameters left out, and
# the values ArduSub keeps itself (the STAT_ counters, the barometers' ground
# pressure, calibrated at every boot), which differ after any session and which
# MAVROS refuses to set.
flatten() { grep -E '^ {4}[A-Z][A-Z0-9_]*: ' "$1" | sed 's/^ *//' |
    grep -vE '^(STAT_[A-Z_]+|BARO[0-9]_GND_PRESS): ' | sort; }

mavros_yaml() { grep 'ros__parameters:' "$1" > /dev/null ||
    die "$1 is not MAVROS's YAML: restore or compare it with hangar up (rov.sh up)"; }

mavros_params_dump() { # file
    ros
    timeout 120 ros2 service call /mavros/param/pull mavros_msgs/srv/ParamPull "{force_pull: true}" \
        | grep 'success=True' > /dev/null || die "MAVROS could not pull the parameters"
    timeout 60 ros2 param dump /mavros/param > "$1"
}

mavros_params_save() {
    need_relay
    mkdir -p "$STATE"
    local f=${1:-$STATE/pixhawk-$(date +%Y%m%d-%H%M%S).yaml}
    mavros_params_dump "$f"
    local n; n=$(flatten "$f" | wc -l)
    some_params "$f" "$n"
    say "$n parameters saved to $f"
}

mavros_params_diff() { # file; exit status 1 when something differs
    need_relay
    [ -f "$1" ] || die "no file $1"
    mavros_yaml "$1"
    local now; now=$(mktemp)
    mavros_params_dump "$now"
    if diff <(flatten "$1") <(flatten "$now") > "$now.diff"; then
        say "every parameter as in $1"; rm -f "$now" "$now.diff"; return 0
    fi
    grep '^<' "$now.diff" | sed 's/^< /  was  /'
    grep '^>' "$now.diff" | sed 's/^> /  now  /'
    rm -f "$now" "$now.diff"
    return 1
}

mavros_params_restore() { # file
    need_relay
    [ -f "$1" ] || die "no file $1"
    mavros_yaml "$1"
    ros
    timeout 10 ros2 topic echo --once /mavros/state 2>/dev/null | grep 'armed: false' > /dev/null \
        || die "the Pixhawk is armed, or MAVROS cannot say: restore only while disarmed"
    local now set; now=$(mktemp); set=$(mktemp)
    mavros_params_dump "$now"
    { echo "/mavros/param:"; echo "  ros__parameters:"
      comm -23 <(flatten "$1") <(flatten "$now") | sed 's/^/    /'; } > "$set"
    local n; n=$(($(wc -l < "$set") - 2))
    if [ "$n" -eq 0 ]; then say "nothing to restore"; rm -f "$now" "$set"; return 0; fi
    say "restoring $n parameters:"; tail -n +3 "$set"
    timeout 120 ros2 param load /mavros/param "$set"
    rm -f "$now" "$set"
    sleep 2
    mavros_params_diff "$1"
}

# ---- commands -------------------------------------------------------------------

cmd_up() {
    for b in hangard hangar hangar-safety hangar-mux hangar-mavlink recorder dawn-patrold; do
        [ -x "$bin/$b" ] || die "missing $bin/$b (set HANGAR_BIN)"
    done
    if [ "$ENCODER" = 1 ]; then
        [ -x "$bin/hangar-encoder" ] || die "missing $bin/hangar-encoder (set HANGAR_BIN)"
        [ ! -d "$RV/etc/hangar" ] || [ -e "$RV/etc/hangar/modules/encoder.module" ] ||
            die "ENCODER=1, but the vehicle root was laid out without the encoder: rov.sh reset first"
    fi
    same_layout "$RV"
    need_serial
    if [ "$BRIDGE" = 1 ]; then
        [ -x "$bin/hangar-bridge" ] || die "missing $bin/hangar-bridge (set HANGAR_BIN)"
        need_key make
        relay_up
        [ -d "$RV/etc/hangar" ] || lay_out "$RV" "$profiles/rov-vehicle"
        start_manager "$RV"
        sleep 1
        cat <<MSG
rov: the vehicle role is up on this Pi, with the bridge (Peer=$GCS_IP)
  vehicle  $RV  safety, mux, mavlink, recorder, bridge, console on http://127.0.0.1:8080
  topside  on the laptop: BRIDGE=1 tools/rov/rov.sh topside up (key: $STATE/bridge.key)
  link     HANGAR_ROOT=$RV $bin/hangar status   (bridge: up <laptop>, rtt ...)
MSG
        return 0
    fi
    same_layout "$RT"
    relay_up
    [ -d "$RV/etc/hangar" ] || lay_out "$RV" "$profiles/rov-vehicle"
    [ -d "$RT/etc/hangar" ] || lay_out "$RT" "$profiles/rov-topside"
    start_manager "$RV" "$RT"
    start_manager "$RT" "$RV"
    sleep 1
    cat <<MSG
rov: hangar is up on this Pi (both roles; the shim stands in for the bridge)
  vehicle  $RV  safety, mux, mavlink, recorder, console on http://127.0.0.1:8080
  topside  $RT  topside_mavlink -> QGroundControl at $TOPSIDE_GCS_IP:14550, console on :8081
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

# On the laptop, BRIDGE=1: the topside role there, the vehicle's across the tether.
cmd_topside() {
    [ "$BRIDGE" = 1 ] || die "rov.sh topside runs the topside role on the laptop: BRIDGE=1 only"
    case "${1:-}" in
        up)
            for b in hangard hangar hangar-mavlink hangar-bridge dawn-patrold; do
                [ -x "$bin/$b" ] || die "missing $bin/$b (set HANGAR_BIN)"
            done
            need_key copy
            same_layout "$RT"
            # QGroundControl on this same machine answers a datagram from any of the
            # machine's own addresses at 127.0.0.1, not at the address it came from
            # (seen at the lab, 2026-11-24: bound to the tether's address the module
            # heard nothing). So the module binds and addresses the loopback.
            TOPSIDE_BIND_IP=127.0.0.1
            TOPSIDE_GCS_IP=127.0.0.1
            [ -d "$RT/etc/hangar" ] || lay_out "$RT" "$profiles/rov-topside"
            start_manager "$RT"
            sleep 1
            cat <<MSG
rov: the topside role is up on this laptop, with the bridge (Peer=$ROV_IP)
  topside  $RT  topside_mavlink -> QGroundControl at $TOPSIDE_GCS_IP:14550, console on :8081
  link     HANGAR_ROOT=$RT $bin/hangar status   (bridge: up <Pi>, rtt ...)
MSG
            ;;
        down) stop_manager "$RT"; say "topside down" ;;
        status)
            if running "$RT/hangard.pid"; then HANGAR_ROOT=$RT "$bin/hangar" status || true
            else echo "no manager"; fi ;;
        reset) stop_manager "$RT"; rm -rf "$RT"; say "topside root removed" ;;
        *) usage 2 ;;
    esac
}

case "${1:-}" in
    up) cmd_up ;;
    topside) cmd_topside "${2:-}" ;;
    down) cmd_down ;;
    status) cmd_status ;;
    reset) cmd_down; rm -rf "$RV" "$RT"; say "roots removed" ;;
    relay)
        case "${2:-}" in
            up) relay_up ;;
            down) relay_down ;;
            status) if running "$STATE/relay.pid"; then echo running; else echo "not running"; fi ;;
            *) usage 2 ;;
        esac ;;
    params)
        case "${2:-}" in
            save) params_save "${3:-}" ;;
            diff) [ -n "${3:-}" ] || usage 2; params_diff "$3" ;;
            restore) [ -n "${3:-}" ] || usage 2; params_restore "$3" ;;
            via) params_via ;;
            *) usage 2 ;;
        esac ;;
    -h|--help|help|'') usage 0 ;;
    *) echo "rov: unknown command $1" >&2; usage 2 ;;
esac
