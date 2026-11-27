#!/usr/bin/env bash
# tools/rov/rov.sh (in a release: share/hangar/rov.sh): hangar on the lab's BlueROV2,
# both roles on the vehicle's Pi (tools/rov/README.md). ArduSub runs on a Pixhawk on
# USB; MAVROS relays it to the vehicle's mavlink module over UDP; the topside role
# sends to QGroundControl on the laptop across the tether. Until the bridge the two
# managers share the Pi and read each other's topics through the same-host shim.
#
#   rov.sh up                 the relay, then both managers (roots laid out once)
#   rov.sh down [--force]     both managers, then the relay; refused unless disarmed
#   rov.sh status             both managers' status, the relay's
#   rov.sh armed              what the guard reads: disarmed, armed or unknown, and why
#   rov.sh spin               the one-second limit tried: a module that never sleeps, under
#                             fifo (needs REALTIME=1 in force); 0 ended by the kernel,
#                             3 real time not in force, 1 it ran on
#   rov.sh reset [--force]    down, then remove both roots (the next up lays them out)
#   rov.sh relay up|down|status  MAVROS alone (down: refused unless disarmed)
#   rov.sh params save [FILE] every Pixhawk parameter to FILE (default under $HANGAR_ROV_STATE)
#   rov.sh params diff FILE   the parameters that differ from FILE now
#   rov.sh params restore FILE  set back the ones that differ, then diff again (disarmed only)
#   rov.sh params via         which way the three go: hangar, mavros or none
#     Through hangar (hangar ardusub-params) whenever the vehicle's manager is up, over
#     MAVROS's UDP or Serial= alike; through MAVROS alone only when hangar is not up.
#     hangar's files are NAME VALUE TYPE lines; MAVROS's, the YAML of ros2 param
#     dump. hangar reads both; the MAVROS fallback, its own YAML only.
#   rov.sh topside up|down|status|reset  on the laptop, with BRIDGE=1: the topside role there
#
# Nothing of hangar is stopped under an armed vehicle (README, "The guard"): down,
# reset and relay down first read safety.state and ardusub.state from the vehicle's
# root and stop nothing, with exit status 4, unless both are being published and say
# disarmed. A manager that runs while one of them is not published is not "disarmed".
# topside down and reset read the bridge's copies: armed is refused; a link already
# down is said and does not keep the laptop's side up. --force (or ROV_FORCE=1) stops
# it all the same and says what it overrode.
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
# REALTIME=1 gives the modules of REALTIME_MODULES (name:priority each) a [Schedule] of
# fifo with locked memory at lay-out, an experiment (README, "Real time");
# ENCODER_SOURCE (uvc:/dev/video2 on the lab's camera) its Source=, set at lay-out;
# MAVLINK=serial lays out the profile's mavlink.conf.serial and mavlink.module.serial:
# the mavlink module on the Pixhawk's serial line itself ($FCU's device and rate), no
# relay, and nothing else may hold the device (modules/mavlink/README.md, "A serial line");
# BRIDGE_PORT and BRIDGE_PEER_PORT (5614 both), the bridge's own UDP port and its peer's:
# two different ones only for both roles on one machine (a rehearsal on loopback).
# HANGAR_ROV_VEHICLE_ROOT and HANGAR_ROV_TOPSIDE_ROOT move the two roots: for the
# guard's own test (tools/rov/tests/test_guard.sh), which must not meet a session's.
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
RV=${HANGAR_ROV_VEHICLE_ROOT:-/dev/shm/hangar-vehicle}
RT=${HANGAR_ROV_TOPSIDE_ROOT:-/dev/shm/hangar-topside}
FORCE=${ROV_FORCE:-0}
# How long the guard waits for a state topic's next message (both are sent at 10 Hz).
GUARD_WAIT=${ROV_GUARD_WAIT:-3}
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
REALTIME=${REALTIME:-0}
REALTIME_MODULES=${REALTIME_MODULES:-safety:60 mux:50 mavlink:50 topside_mavlink:50 bridge:40}
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

usage() { sed -n '2,50p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
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
    # Real time is opt-in, and an experiment: the profiles' manifests ask for none
    # (docs/00: not required). REALTIME=1 gives each module of REALTIME_MODULES that is
    # laid out a [Schedule] of fifo at its priority, with locked memory. Whether the
    # manager can grant it is another matter (docs/13 "Real-time scheduling without
    # root"); hangar status and hangar check say what is in force.
    if [ "$REALTIME" = 1 ]; then
        local spec
        for spec in $REALTIME_MODULES; do
            f=$root/etc/hangar/modules/${spec%%:*}.module
            [ -e "$f" ] || continue
            grep -q '^\[Schedule\]' "$f" && continue
            printf '\n[Schedule]\nPolicy=fifo\nPriority=%s\nLockMemory=yes\n' "${spec##*:}" >> "$f"
        done
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

# ---- the guard: hangar is not stopped under an armed vehicle ------------------------
#
# On 2026-11-24 `rov.sh down` was run on the Pi while the vehicle was armed. The mavlink
# module sent neutral and a disarm as it stopped, so nothing came of it; that is the
# module's last resort and not a way to end a session. Since then every command here
# that stops a manager, or the relay under one, asks first.

# The newest message of a state topic under a root, as hangar echo's JSON line. Nothing,
# unless two messages arrive within $GUARD_WAIT s: the second is one its module sent
# while we looked, so what is read is the state now, not the last word of a module that
# died or hangs (its topic file, and the last message in it, outlive it).
state_now() { # root topic fields
    local out
    out=$(HANGAR_ROOT=$1 timeout "$GUARD_WAIT" "$bin/hangar" echo "$2" --json --count 2 --fields "$3" 2>/dev/null) || return 1
    [ "$(grep -c '^{' <<< "$out")" -eq 2 ] || return 1
    tail -n 1 <<< "$out"
}
json_num() { sed -n "s/.*\"$1\":\([0-9][0-9]*\).*/\1/p" <<< "$2"; }

# What the two topics under a root say: STATE_WORD disarmed, armed or unknown; STATE_WHY
# what was read, or why nothing could be; STATE_NOTE what "disarmed" leaves open.
#
# Disarmed is: safety.state disarmed, or in a failsafe whose action is neutral (the
# outputs off: docs/07, the state after a tether or a module failsafe, in which ArduSub
# was disarmed and which a restart keeps), AND ardusub.state armed 0. Arming, armed, a
# failsafe that drives (surface, hold), ArduSub armed or still disarming: not disarmed.
read_state() { # root
    local s a st en arm rx sname aname
    STATE_WORD=unknown STATE_WHY="" STATE_NOTE=""
    if ! s=$(state_now "$1" safety.state state,outputs_enabled); then
        STATE_WHY="safety.state is not being published under $1: the safety module is not running, or hangs"
        return 0
    fi
    if ! a=$(state_now "$1" ardusub.state armed,last_rx_ns); then
        STATE_WHY="ardusub.state is not being published under $1: the mavlink module is not running, or hangs"
        return 0
    fi
    st=$(json_num state "$s"); en=$(json_num outputs_enabled "$s")
    arm=$(json_num armed "$a"); rx=$(json_num last_rx_ns "$a")
    if [ -z "$st" ] || [ -z "$en" ] || [ -z "$arm" ] || [ -z "$rx" ]; then
        STATE_WHY="safety.state and ardusub.state under $1 could not be read (hangar echo said: $s $a)"
        return 0
    fi
    case $st in 0) sname=disarmed ;; 1) sname=armed ;; 3) sname=arming ;;
        2) if [ "$en" = 0 ]; then sname="failsafe, outputs off"; else sname="failsafe, outputs ON"; fi ;;
        *) sname="state $st" ;; esac
    case $arm in 0) aname=disarmed ;; 1) aname=armed ;; 2) aname=disarming ;; *) aname="armed $arm" ;; esac
    STATE_WHY="safety: $sname; ArduSub: $aname"
    if { [ "$st" = 0 ] || { [ "$st" = 2 ] && [ "$en" = 0 ]; }; } && [ "$arm" = 0 ]; then
        STATE_WORD=disarmed
        [ "$rx" != 0 ] || STATE_NOTE="the mavlink module has not heard ArduSub since it started: \"disarmed\" is hangar's side only"
    else
        STATE_WORD=armed
    fi
}

refused() { # lines...: to stderr, exit status 4
    local l
    for l in "$@"; do echo "rov: $l" >&2; done
    exit 4
}

# guard vehicle|topside ROOT WHAT: returns if WHAT may be done to the manager under ROOT.
#
# vehicle: ROOT is the vehicle's own. No manager there: nothing of hangar drives
# anything, nothing to guard. Armed, or not known: refused.
#
# topside: ROOT is the topside's on the laptop, the pilot's end of the tether, and the
# state is the bridge's copy of the vehicle's. Armed: refused, since stopping the
# pilot's end under an armed vehicle is the tether failsafe fired on purpose (neutral
# and a disarm some 3 s later, the sticks dead meanwhile). Not known: the copies stopped
# arriving, so the link is down already and the vehicle has acted on that by itself, or
# will; this side staying up changes nothing for it, and a laptop that cannot stop its
# own topside without the vehicle's leave would be stuck whenever the tether is out. It
# is said, and allowed.
guard() { # role root what
    local role=$1 root=$2 what=$3 hg
    running "$root/hangard.pid" || return 0
    read_state "$root"
    hg="HANGAR_ROOT=$root $bin/hangar"
    if [ "$STATE_WORD" = disarmed ]; then
        [ -z "$STATE_NOTE" ] || say "$STATE_NOTE"
        return 0
    fi
    if [ "$FORCE" = 1 ]; then
        say "--force: $what all the same ($STATE_WHY)"
        return 0
    fi
    if [ "$STATE_WORD" = armed ] && [ "$role" = vehicle ]; then
        refused "NOT DONE ($what): the vehicle is not disarmed ($STATE_WHY)." \
            "  Disarm first: QGroundControl, the console, or $hg call safety.disarm" \
            "  It has to come down as it is: the same command with --force (the mavlink module" \
            "  sends neutral and a disarm as it stops; that is its last resort, not a way to end)."
    elif [ "$STATE_WORD" = armed ]; then
        refused "NOT DONE ($what): the vehicle is not disarmed ($STATE_WHY), and this is its pilot's" \
            "  end of the tether: without it the vehicle goes to its tether failsafe." \
            "  Disarm first: QGroundControl, the console, or $hg call safety.disarm" \
            "  It has to come down as it is: the same command with --force."
    elif [ "$role" = vehicle ]; then
        refused "NOT DONE ($what): a manager runs under $root and whether the vehicle is disarmed" \
            "  cannot be read: $STATE_WHY." \
            "  That is not \"disarmed\". Look: $hg status; QGroundControl says what ArduSub is." \
            "  Once it is known to be disarmed, or it has to come down anyway: the same command with --force."
    else
        say "the vehicle's state does not reach this laptop ($STATE_WHY):"
        say "  the link is down already; $what changes nothing for the vehicle"
    fi
}

# The root down and reset would stop a manager under: the vehicle's if one runs here;
# on the laptop of a BRIDGE=1 session, the topside's (a root laid out with the bridge).
guard_down() { # what
    if running "$RV/hangard.pid"; then guard vehicle "$RV" "$1"
    elif [ -e "$RT/etc/hangar/modules/bridge.module" ]; then guard topside "$RT" "$1"
    fi
}

cmd_armed() {
    local root=$RV
    if ! running "$RV/hangard.pid" && [ -e "$RT/etc/hangar/modules/bridge.module" ]; then root=$RT; fi
    running "$root/hangard.pid" || { echo "no manager"; return 0; }
    read_state "$root"
    echo "$STATE_WORD: $STATE_WHY${STATE_NOTE:+ ($STATE_NOTE)}"
    case $STATE_WORD in disarmed) return 0 ;; armed) return 4 ;; *) return 5 ;; esac
}

# The one-second limit, tried (docs/13 "Real-time scheduling without root"): a module
# that never sleeps, started under fifo at a priority below every module of hangar's.
# The kernel must end it within about a second (RLIMIT_RTTIME, which the manager sets
# on every real-time module). Where the manager cannot grant fifo there is nothing to
# try, and the module is not left spinning. Exit 0 ended by the kernel, 3 real time
# not in force, 1 it ran on (stopped here: that is the finding).
cmd_spin() {
    running "$RV/hangard.pid" || die "spin: no vehicle manager: rov.sh up first (REALTIME=1)"
    [ -x "$bin/spin" ] || die "spin: $bin/spin is not in this release"
    guard vehicle "$RV" "starting a module that never sleeps"
    local f=$RV/etc/hangar/modules/spin.module rc=1 i line
    printf '[Module]\nExec=%s\n[Restart]\nPolicy=never\n[Schedule]\nPolicy=fifo\nPriority=%s\n' \
        "$bin/spin" "${SPIN_PRIORITY:-5}" > "$f"
    hg_vehicle reload > /dev/null || { rm -f "$f"; die "spin: the manager refused the manifest"; }
    hg_vehicle start spin > /dev/null 2>&1 || true
    sleep 0.3
    line=$(hg_vehicle show spin 2>/dev/null | grep '^in force' || true)
    case $line in
        *fifo*)
            say "spin: running under fifo (${line#in force }); the kernel should end it within a second"
            for i in 1 2 3 4 5 6 7 8 9 10; do
                sleep 0.5
                hg_vehicle show spin 2>/dev/null | grep -q '^in force   not running' && { rc=0; break; }
            done
            if [ $rc = 0 ]; then say "spin: ended after about $((i / 2)).$((i % 2 * 5)) s: the limit holds"
            else say "spin: STILL RUNNING after 5 s under fifo: the limit did not end it (stopping it now)"; fi ;;
        *)
            rc=3
            say "spin: real time is not in force here (${line:-no reading}): nothing to try" ;;
    esac
    hg_vehicle stop spin > /dev/null 2>&1 || true
    grep ' spin: ' "$RV/hangard.log" 2>/dev/null | tail -6 | sed 's/^/rov:   /'
    rm -f "$f"
    hg_vehicle reload > /dev/null || true
    return $rc
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

cmd_down() { # what, for the guard
    guard_down "${1:-stopping hangar}"
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
        down) guard topside "$RT" "stopping the topside role"; stop_manager "$RT"; say "topside down" ;;
        status)
            if running "$RT/hangard.pid"; then HANGAR_ROOT=$RT "$bin/hangar" status || true
            else echo "no manager"; fi ;;
        reset)
            guard topside "$RT" "stopping the topside role and removing its root"
            stop_manager "$RT"; rm -rf "$RT"; say "topside root removed" ;;
        *) usage 2 ;;
    esac
}

# --force, wherever it stands among the words.
words=()
for w in "$@"; do
    if [ "$w" = --force ]; then FORCE=1; else words+=("$w"); fi
done
set -- ${words[@]+"${words[@]}"}

case "${1:-}" in
    up) cmd_up ;;
    topside) cmd_topside "${2:-}" ;;
    down) cmd_down ;;
    status) cmd_status ;;
    armed) cmd_armed ;;
    spin) cmd_spin ;;
    reset) cmd_down "stopping hangar and removing its roots"; rm -rf "$RV" "$RT"; say "roots removed" ;;
    relay)
        case "${2:-}" in
            up) relay_up ;;
            down)
                # The relay is the mavlink module's only way to ArduSub: without it
                # hangar can neither drive nor disarm.
                if running "$STATE/relay.pid"; then guard vehicle "$RV" "stopping the relay"; fi
                relay_down ;;
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
