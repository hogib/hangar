#!/usr/bin/env bash
# tools/rov/lab.sh: a lab session with the lab's BlueROV2, from the laptop, in five
# commands (tools/rov/QUICKSTART.md; issue #4). It drives the vehicle's Pi over SSH and
# keeps what it changed in a session file, so that down puts everything back.
#
#   lab.sh deploy [--realtime] [TARBALL]
#                                    copy a release to the Pi, check it, install it,
#                                    set the Pi's clock (default: the newest aarch64
#                                    tarball under out/release/); --realtime: sudo setcap
#                                    on the installed hangard, so that a manager that is
#                                    not root can give a module what its [Schedule] asks
#   lab.sh preflight [--measure] [--video] [--bridge] [--serial]
#                                    what both machines are doing besides hangar: the
#                                    package system, clock, heat, room, ports, the lab's
#                                    services, real time. One line each, ok, warn or
#                                    FAIL, with the command that puts it right; nothing
#                                    is changed. --measure: what would spoil a number
#   lab.sh up [--video] [--bridge] [--serial] [--no-battery-check] [--no-preflight]
#             [--realtime]
#                                    hangar up on the Pi, ArduSub's parameters saved
#                                    and set, the pre-arm check; consoles tunnelled;
#                                    --video: the camera's own H.264, the lab's
#                                    electric-eye stopped, the laptop's video started;
#                                    --bridge: the topside role on this laptop, joined
#                                    to the vehicle by the tether bridge (the tether
#                                    trigger on: arming needs the link);
#                                    --serial: the mavlink module on the Pixhawk's
#                                    serial line itself, no MAVROS relay;
#                                    --realtime: an experiment: safety, mux, mavlink and
#                                    the bridge under fifo with locked memory, if the
#                                    manager may give it (deploy --realtime)
#   lab.sh status                    what runs where, armed or not, temperature
#   lab.sh bench [SECONDS]           CPU, memory, frame rates on both, to RESULTS_DIR,
#                                    with the preflight before and after beside them
#   lab.sh camera [FIELD=VALUE ...]  the camera's controls with --video: hangar's encoder
#                                    holds the camera, and they are its parameters
#                                    (brightness=120, auto_exposure=0 exposure_us=8000,
#                                    gain=30; -1 leaves one to the camera). Alone: the
#                                    values in force.
#   lab.sh down [--force]            everything put back as found: disarmed first if
#                                    it is armed, and nothing stopped unless the
#                                    vehicle then reads disarmed (--force: all the same)
#   lab.sh freeze                    the Pi held still for measurements, under one sudo:
#                                    the package system's timers kept from starting until
#                                    the next boot, a running upgrade asked to end and
#                                    waited for, the lab's own services stopped, the
#                                    governor performance, the clock set. What was found
#                                    is written down first, and the commands are shown
#   lab.sh thaw                      all of that put back as it was found (a reboot of
#                                    the Pi undoes a freeze too). The last thing before
#                                    leaving: status and down say while the Pi is frozen
#   lab.sh baselines [--debs DIR] [TARBALL]
#                                    the other systems of bench/pairs, to the Pi: the
#                                    archive of tools/baselines.sh unpacked in
#                                    ~/hangar-baselines (default: the newest under
#                                    out/baselines/), each node asked for its version
#                                    there; --debs DIR: the .deb files of
#                                    tools/baselines-debs.sh checked and installed with
#                                    dpkg -i under one sudo, the command shown first.
#                                    Nothing is downloaded or compiled on the Pi
#
# Nothing here stops hangar under an armed vehicle (tools/rov/README.md, "The guard"):
# up, which lays hangar out afresh, refuses while it is armed and disarms nothing.
# up runs the preflight first and stops at a FAIL (--no-preflight: all the same).
#
# Configuration: tools/rov/lab.conf (~/.config/hangar/lab.conf first; the environment
# first of all). LAB_HOST=local rehearses it all on this laptop, without the vehicle.
set -uo pipefail

here=$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")
repo=$(realpath -- "$here/../..")
[ -f "$HOME/.config/hangar/lab.conf" ] && . "$HOME/.config/hangar/lab.conf"
. "$here/lab.conf"

# Rehearsal on this laptop: everything local, no Pixhawk (rov.sh's RELAY=0), the tether
# addresses on loopback, an install of its own.
if [ "$LAB_HOST" = local ]; then
    LAPTOP_IP=127.0.0.1
    [ "$ROV_ROOT" = hangar-root ] && ROV_ROOT=hangar-labtest
    : "${RELAY:=0}"
fi
: "${RELAY:=1}"
ROV_IP=$LAB_HOST
[ "$LAB_HOST" = local ] && ROV_IP=127.0.0.1
TARGET=$LAB_USER@$LAB_HOST
SESSION=$LAB_STATE_DIR/session
# The two roots, as rov.sh names them. With --bridge the topside's is on this laptop.
RV=/dev/shm/hangar-vehicle
RT=/dev/shm/hangar-topside
# The bridge's UDP port on each end: one port on the tether; rehearsing, both ends are
# this laptop and the topside's is the next one.
V_PORT=$BRIDGE_PORT
T_PORT=$BRIDGE_PORT
[ "$LAB_HOST" = local ] && T_PORT=$((BRIDGE_PORT + 1))
HELLION=$repo/builddir/hellion
EEYE=$repo/builddir/eeye
mkdir -p "$LAB_STATE_DIR"
chmod 700 "$LAB_STATE_DIR"
SSH_OPTS=(-o ControlMaster=auto -o "ControlPath=$LAB_STATE_DIR/ssh-%C" -o ControlPersist=30m
          -o ConnectTimeout=6 -o ServerAliveInterval=5 -o ServerAliveCountMax=3)

usage() { sed -n '2,68p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
say() { echo "lab: $*"; }
warn() { echo "lab: $*" >&2; }
die() { warn "$*"; exit 1; }
is_local() { [ "$LAB_HOST" = local ]; }

# ---- the session file: what up changed, for down -----------------------------------

sess_get() { [ -f "$SESSION" ] && sed -n "s/^$1=//p" "$SESSION" | tail -1; }
sess_set() {
    local tmp; tmp=$(mktemp)
    { [ -f "$SESSION" ] && grep -v "^$1=" "$SESSION"; echo "$1=$2"; } > "$tmp"
    mv "$tmp" "$SESSION"
}

# ---- the Pi --------------------------------------------------------------------------

# A script on stdin, run on the Pi (or here, rehearsing) with NAME=value settings;
# ROOT is always set to hangar's install and HANGAR_ROOT to the vehicle's root.
remote() {
    local env=(ROOT="$ROV_ROOT" HANGAR_ROOT=/dev/shm/hangar-vehicle "$@")
    if is_local; then
        env "${env[@]}" bash -s
    else
        ssh "${SSH_OPTS[@]}" "$TARGET" "$(printf '%q ' env "${env[@]}") bash -s"
    fi
}

# The scripts start by finding the install: $ROOT relative to the home, or absolute.
PRELUDE='case $ROOT in /*) R=$ROOT ;; *) R=$HOME/$ROOT ;; esac
B=$R/bin; ROV=$R/share/hangar/rov.sh'

# One script on the Pi, after the prelude: rsh [NAME=value...] 'script'.
rsh() {
    local script=${*: -1}
    remote "${@:1:$#-1}" <<< "$PRELUDE"$'\n'"$script"
}

# A command as root on the Pi. sudo asks for the password on this terminal; it is never
# stored. Rehearsing, it is skipped and said.
remote_sudo() {
    if is_local; then say "rehearsal, skipped: sudo $*"; return 0; fi
    if [ -t 0 ]; then
        ssh -t "${SSH_OPTS[@]}" "$TARGET" "sudo $(printf '%q ' "$@")"
    else
        ssh "${SSH_OPTS[@]}" "$TARGET" "sudo -n $(printf '%q ' "$@")" ||
            { warn "sudo on the Pi needs a password and there is no terminal to ask on: run lab.sh from a terminal"; return 1; }
    fi
}

copy_to() { # local-file remote-dir (relative to the Pi's home)
    if is_local; then mkdir -p "$HOME/$2" && cp "$1" "$HOME/$2/"
    else scp -q "${SSH_OPTS[@]}" "$1" "$TARGET:$2/"; fi
}
copy_from() { # remote-file local-dir
    if is_local; then cp "$HOME/$1" "$2/"
    else scp -q "${SSH_OPTS[@]}" "$TARGET:$1" "$2/"; fi
}

reachable() {
    is_local && return 0
    ssh "${SSH_OPTS[@]}" -o BatchMode=no "$TARGET" true 2>/dev/null ||
        { warn "cannot reach $TARGET over SSH (the tether? the address in lab.conf? ssh-copy-id done?)"; return 1; }
}

hangar_running() { # 0 if a manager runs on the Pi
    remote <<'EOF'
for p in /dev/shm/hangar-vehicle/hangard.pid /dev/shm/hangar-topside/hangard.pid; do
    [ -f "$p" ] && kill -0 "$(cat "$p")" 2>/dev/null && exit 0
done
exit 1
EOF
}

# A copy of the session file on the Pi (~/hangar-rov/lab-session), sent whenever up has
# written down something to undo there: the laptop's copy lives in ~/.cache, and a down
# without it would know of nothing to put back. down reads the Pi's when its own is gone.
sess_push() {
    [ -f "$SESSION" ] || return 0
    rsh DATA="$(base64 -w0 < "$SESSION")" \
        'mkdir -p "$HOME/hangar-rov" && echo "$DATA" | base64 -d > "$HOME/hangar-rov/lab-session"' ||
        warn "the session could not be copied to the Pi: do not lose $SESSION before lab.sh down"
}
sess_pull() { # 0 if the Pi had one; only name=value lines of plain characters are taken
    local data
    data=$(rsh 'cat "$HOME/hangar-rov/lab-session" 2>/dev/null' | grep -E '^[a-z_]+=[A-Za-z0-9 _./:+-]*$')
    [ -n "$data" ] || return 1
    printf '%s\n' "$data" > "$SESSION"
}

# ---- rov.sh, on the Pi and here ----------------------------------------------------------

# The settings rov.sh on the Pi runs with, in ROV_ENV: rov_env BRIDGE SERIAL (0 or 1 each).
rov_env() {
    ROV_ENV=(ROV_IP="$ROV_IP" GCS_IP="$LAPTOP_IP" RELAY="$RELAY" FCU="$FCU"
             BRIDGE="$1" BRIDGE_PORT="$V_PORT" BRIDGE_PEER_PORT="$T_PORT" REALTIME="${LAB_REALTIME:-0}")
    if [ "$2" = 1 ]; then ROV_ENV+=(MAVLINK=serial RELAY=0); fi
}

# The topside role on this laptop, for --bridge: the checkout's rov.sh and builddir/.
topside() { # up|down|status|reset
    ROV_FORCE="${LAB_FORCE:-0}" BRIDGE=1 ROV_IP="$ROV_IP" GCS_IP="$LAPTOP_IP" BRIDGE_PORT="$T_PORT" BRIDGE_PEER_PORT="$V_PORT" \
        TOPSIDE_HELLION_URL="http://127.0.0.1:$HELLION_CONTROL_PORT/" \
        bash "$here/rov.sh" topside "$1"
}
topside_hangar() { HANGAR_ROOT=$RT "$repo/builddir/hangar" "$@"; }
# A topside root laid out with the bridge is this laptop's own (the shim's, rehearsing,
# has no bridge in it).
topside_laid_out() { [ -e "$RT/etc/hangar/modules/bridge.module" ]; }
bridge_session() { [ "$(sess_get bridge)" = 1 ] || topside_laid_out; }
topside_line() { topside_hangar status 2>/dev/null | awk -v m="$1" '$1 == m { $1 = $1; print }'; }

# What the vehicle's own topics say, by the Pi's rov.sh (its guard's reading): VSTATE
# disarmed, armed or unknown, VWHY the reading. A release on the Pi from before rov.sh
# had the word: the safety and mavlink modules' status lines instead, which say the same
# of a module that runs and nothing of one that hangs.
vehicle_state() {
    local out
    out=$(rsh 'if grep -q "^cmd_armed()" "$ROV"; then "$ROV" armed 2>&1; exit 0; fi
st=$("$B/hangar" status 2>/dev/null)
s=$(awk '\''$1 == "safety" { $1 = $1; print }'\'' <<< "$st"); m=$(awk '\''$1 == "mavlink"'\'' <<< "$st" | grep -oE "ardusub [a-z]+")
old="by the status lines: the Pi has a rov.sh from before the guard"
case "$s|$m" in
    "safety ready "*" disarmed"*"|ardusub disarmed") echo "disarmed: $s; $m ($old)" ;;
    "safety ready "*"|ardusub "*) echo "armed: $s; $m ($old)" ;;
    *) echo "unknown: safety: ${s:-no status}; mavlink: ${m:-no status} ($old)" ;;
esac') || out=""
    case $out in
        disarmed:*|"no manager") VSTATE=disarmed ;;
        armed:*) VSTATE=armed ;;
        *) VSTATE=unknown ;;
    esac
    VWHY=${out#*: }
    [ -n "$out" ] || VWHY="the Pi did not answer"
}

# What the two bridges compare before they carry anything (docs/08, config_match): the
# msg/ tree and the bridge's configuration but for each machine's own lines.
BUILD_SUMS='echo "msg $(cd "$MSGDIR" 2>/dev/null && LC_ALL=C ls *.msg | LC_ALL=C xargs sha256sum | sha256sum | cut -c1-16)"
echo "conf $(grep -vE "^[[:space:]]*(#|\$)|^(Peer|Key|MsgDir|Port|DiscoveryPort)=" "$CONF" 2>/dev/null | sha256sum | cut -c1-16)"'

# Both ends the same build, or up stops before anything is started: the bridge would
# come up and carry nothing. The same commit is the tested case; a different commit
# with the same message types and configuration is carried, and said.
same_build() {
    local theirs ours tc oc
    theirs=$(rsh 'MSGDIR=$R/share/hangar/msg CONF=$R/share/hangar/profiles/rov-vehicle/bridge.conf
'"$BUILD_SUMS"'
echo "commit $(sed -n "s/^commit //p" "$R/MANIFEST" 2>/dev/null | cut -c1-10)"') || return 1
    ours=$(MSGDIR=$repo/msg CONF=$repo/profiles/rov-topside/bridge.conf bash -c "$BUILD_SUMS")
    tc=$(sed -n 's/^commit //p' <<< "$theirs"); tc=${tc:-unknown}
    oc=$(git -C "$repo" rev-parse --short=10 HEAD 2>/dev/null || echo unknown)
    git -C "$repo" diff --quiet HEAD -- 2>/dev/null || oc="$oc, with changes not committed"
    if [ "$(grep -v '^commit' <<< "$theirs")" != "$ours" ]; then
        warn "the two ends are NOT THE SAME BUILD: the bridge would carry nothing between them (config_match)."
        warn "  the Pi's release: commit $tc; this checkout: commit $oc"
        [ "$(grep '^msg' <<< "$theirs")" = "$(grep '^msg' <<< "$ours")" ] || warn "  their msg/ trees differ"
        [ "$(grep '^conf' <<< "$theirs")" = "$(grep '^conf' <<< "$ours")" ] ||
            warn "  profiles/rov-vehicle/bridge.conf there and profiles/rov-topside/bridge.conf here differ in more than Peer="
        warn "  a release of this checkout on the Pi (tools/release.sh, lab.sh deploy), or this checkout at the"
        warn "  Pi's commit (make -j8 all after it). Nothing was started."
        return 1
    fi
    if [ "$tc" = "$oc" ]; then
        say "both ends are the same build (commit $oc)"
    else
        say "the Pi's release is commit $tc, this checkout $oc: the message types and the bridge's"
        say "  configuration are the same, so the bridge carries; the same commit on both is the tested case"
    fi
}

# Both bridges up and agreeing, within 30 s. config_match false is said as what it is.
wait_link() {
    local t=0 v="" l="" up='^bridge ready .* up [^ ]+:[0-9]+, rtt '
    while [ "$t" -lt 30 ]; do
        v=$(status_of bridge); l=$(topside_line bridge)
        if [[ "$v$l" == *"config_match false"* ]]; then
            warn "the link is up and the bridge carries NOTHING: the two ends are not the same build (config_match false)."
            warn "  the Pi:      ${v:-no status}"
            warn "  this laptop: ${l:-no status}"
            warn "  the same commit on both: tools/release.sh and lab.sh deploy, make -j8 all here"
            return 1
        fi
        if [[ $v =~ $up ]] && [[ $l =~ $up ]]; then
            say "the link is up: $(sed 's/.* \(up [^ ]*:[0-9]*, rtt [^,]*, offset [^,]*\).*/\1/' <<< "$l")"
            return 0
        fi
        sleep 2; t=$((t + 2))
    done
    warn "the link did not come up in 30 s."
    warn "  the Pi:      ${v:-no status}"
    warn "  this laptop: ${l:-no status}"
    warn "  UDP $V_PORT must pass this laptop's firewall from the Pi; this laptop must have one address"
    warn "  on the tether, $LAPTOP_IP (LAPTOP_IP in lab.conf); the bridges' logs are"
    warn "  <root>/run/hangar/log/bridge.log on both"
    return 1
}

# ---- the Pixhawk's serial line, for --serial ----------------------------------------------

# Who else holds the device: every process with it open that this user can see, and any
# mavros_node at all. OWN=1 leaves out what up stops by itself (hangar's own modules, its
# relay, the units in LAB_SERIAL_SERVICES); OWN=0 is asked after those are stopped.
# Status 3 with a line a holder, 4 if the device is not there or cannot be opened.
serial_holders() { # own
    remote DEV="${FCU%%:*}" OWN="$1" SKIP_UNITS="$LAB_SERIAL_SERVICES" <<'EOF'
case $ROOT in /*) R=$ROOT ;; *) R=$HOME/$ROOT ;; esac
[ -e "$DEV" ] || { echo "  no $DEV on the Pi: the Pixhawk's USB cable, or FCU in lab.conf"; exit 4; }
[ -r "$DEV" ] && [ -w "$DEV" ] || { echo "  $(id -un) on the Pi cannot open $DEV: the group that owns it (dialout)"; exit 4; }
real=$(readlink -f "$DEV")
relay=$(cat "$HOME/hangar-rov/relay.pid" 2>/dev/null)
rc=0
for p in $( { find /proc/[0-9]*/fd -lname "$real" 2>/dev/null | cut -d/ -f3; pgrep -x mavros_node; } | sort -un); do
    [ -d "/proc/$p" ] || continue
    unit=$(sed -n 's|.*/\([^/]*\)\.service$|\1|p' "/proc/$p/cgroup" 2>/dev/null | head -1)
    if [ "$OWN" = 1 ]; then
        case $(readlink "/proc/$p/exe" 2>/dev/null) in "$R"/*) continue ;; esac
        [ -n "$relay" ] && [ "$(ps -o sid= -p "$p" 2>/dev/null | tr -d ' ')" = "$relay" ] && continue
        for u in $SKIP_UNITS; do [ "${u%.service}" = "$unit" ] && continue 2; done
    fi
    echo "  pid $p, $(cat "/proc/$p/comm" 2>/dev/null)${unit:+, unit $unit}: $(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | cut -c1-100)"
    rc=3
done
exit $rc
EOF
}

serial_refused() { # status holders
    if [ "$1" = 4 ]; then warn "--serial: the Pixhawk's serial line cannot be used:"; warn "$2"; return; fi
    warn "--serial: ${FCU%%:*} on the Pi is held by something else, and the mavlink module needs it alone:"
    warn "$2"
    warn "  Stop it on the Pi and run up again. A systemd unit (the lab's own MAVROS) can be left to"
    warn "  lab.sh: name it in LAB_SERIAL_SERVICES (lab.conf) and up stops it, as it stops"
    warn "  \"$LAB_SERVICES\" for --video, and down starts it again."
}

# The lab's units stopped for this session, written down before the stop: one that fails
# half-way has stopped some of them, and down starts them all (starting one that runs
# changes nothing).
stop_services() { # unit...
    local all s; all=$(sess_get services_stopped)
    for s in "$@"; do [[ " $all " == *" $s "* ]] || all="${all:+$all }$s"; done
    sess_set services_stopped "$all"
    sess_push
    remote_sudo systemctl stop "$@"
}

# ---- freeze and thaw -----------------------------------------------------------------

# tools/rov/freeze.sh on the Pi (README "Freeze and thaw"). Rehearsing, it runs here
# against a Pi that is a directory (tests/fake-pi.sh): nothing of this laptop is changed.
FREEZE_SH=$here/freeze.sh
FROZEN_FILE=/run/hangar-frozen
FREEZE_SEAMS=()
if is_local; then
    FAKE_PI=$LAB_STATE_DIR/freeze-rehearsal
    FROZEN_FILE=$FAKE_PI/run/hangar-frozen
fi
freeze_args() { # the settings, in FREEZE_ARGS
    FREEZE_ARGS=(SERVICES="$LAB_SERVICES" GOVERNOR="$LAB_FREEZE_GOVERNOR" TIMERS="$LAB_FREEZE_TIMERS"
                 UNITS="$LAB_FREEZE_UNITS" WAIT="$LAB_FREEZE_WAIT")
    if is_local; then
        [ -d "$FAKE_PI" ] || bash "$here/tests/fake-pi.sh" make "$FAKE_PI"
        readarray -t FREEZE_SEAMS < <(bash "$here/tests/fake-pi.sh" seams "$FAKE_PI")
    fi
}
# The record of a frozen Pi, on standard output; nothing and status 1 if it is not.
frozen_record() { rsh FILE="$FROZEN_FILE" 'cat "$FILE" 2>/dev/null'; }
frozen_since() { frozen_record | sed -n 's/^frozen_at //p' | head -1; }
# The script onto the Pi, its path there in FREEZE_REMOTE.
freeze_send() {
    rsh DATA="$(base64 -w0 < "$FREEZE_SH")" \
        'mkdir -p "$HOME/hangar-rov" && echo "$DATA" | base64 -d > "$HOME/hangar-rov/freeze.sh"' || return 1
    FREEZE_REMOTE=$(rsh 'echo "$HOME/hangar-rov/freeze.sh"')
}

cmd_freeze() {
    [ $# -eq 0 ] || usage 2
    reachable || exit 1
    freeze_args
    local rc
    if is_local; then
        say "rehearsal: a Pi that is a directory ($FAKE_PI); nothing of this laptop is changed"
        bash "$FREEZE_SH" plan "${FREEZE_ARGS[@]}" "${FREEZE_SEAMS[@]}"
        bash "$FREEZE_SH" freeze "${FREEZE_ARGS[@]}" "${FREEZE_SEAMS[@]}"; rc=$?
    else
        freeze_send || die "could not copy freeze.sh to the Pi"
        local theirs ours
        theirs=$(remote <<< 'date -u +%s') || die "the Pi did not answer"
        ours=$(date -u +%s)
        FREEZE_ARGS+=(CLOCK_OURS="$ours" CLOCK_THEIRS="$theirs")
        # What will be run as root, read from the Pi as it is now, before the password.
        rsh "$(printf '%q ' bash "$FREEZE_REMOTE" plan "${FREEZE_ARGS[@]}")"
        say "sudo on the Pi, once, for the commands above"
        remote_sudo bash "$FREEZE_REMOTE" freeze "${FREEZE_ARGS[@]}"; rc=$?
    fi
    if frozen_record > "$LAB_STATE_DIR/frozen.tmp" && [ -s "$LAB_STATE_DIR/frozen.tmp" ]; then
        mv "$LAB_STATE_DIR/frozen.tmp" "$LAB_STATE_DIR/frozen"
        say "the record is on the Pi ($FROZEN_FILE) and here ($LAB_STATE_DIR/frozen)"
        say "lab.sh thaw before leaving. A reboot of the Pi undoes the freeze too, record and all"
    else
        rm -f "$LAB_STATE_DIR/frozen.tmp"
        warn "the Pi does not read frozen: freeze did not run (the password? lab.sh freeze again)"
        exit 1
    fi
    [ "$rc" = 0 ] || warn "freeze ended with status $rc: read what it said above; lab.sh preflight --measure says what is left"
    return "$rc"
}

cmd_thaw() {
    [ $# -eq 0 ] || usage 2
    reachable || exit 1
    if [ -z "$(frozen_since)" ]; then
        say "the Pi is not frozen: nothing to put back"
        [ ! -f "$LAB_STATE_DIR/frozen" ] ||
            { say "  (it was frozen at $(sed -n 's/^frozen_at //p' "$LAB_STATE_DIR/frozen"): it has been thawed or rebooted since)"; rm -f "$LAB_STATE_DIR/frozen"; }
        return 0
    fi
    # thaw starts the lab's own services again: with a session up that would give the
    # camera and the serial line back under hangar's hands.
    hangar_running && die "hangar runs on the Pi: lab.sh down first, then thaw (thaw starts the lab's own services again)"
    freeze_args
    if is_local; then
        bash "$FREEZE_SH" thaw "${FREEZE_SEAMS[@]}" || exit 1
    else
        freeze_send || die "could not copy freeze.sh to the Pi"
        say "sudo on the Pi, once, to put back what freeze wrote down"
        remote_sudo bash "$FREEZE_REMOTE" thaw || exit 1
    fi
    [ -z "$(frozen_since)" ] || die "the Pi still reads frozen: thaw did not run (the password? lab.sh thaw again)"
    rm -f "$LAB_STATE_DIR/frozen"
    say "thawed"
}

# One line for status and down while the Pi is frozen; nothing if it is not.
frozen_notice() {
    local since; since=$(frozen_since)
    [ -n "$since" ] || return 1
    warn "THE PI IS FROZEN (since $since): its apt timers are masked, the lab's own services"
    warn "  stopped, the governor set. lab.sh thaw before leaving (a reboot undoes it too)."
}

# ---- the clock -----------------------------------------------------------------------

# The Pi has no clock of its own and nothing sets it over the tether: every log and
# recording stamp is wrong until it is set.
set_clock() {
    is_local && return 0
    local theirs ours skew
    theirs=$(remote <<< 'date -u +%s') || return 1
    ours=$(date -u +%s)
    skew=$(( theirs > ours ? theirs - ours : ours - theirs ))
    if [ "$skew" -le 2 ]; then say "the Pi's clock is right (${skew} s off)"; return 0; fi
    say "the Pi's clock is ${skew} s off: setting it from this laptop's"
    # date's output is dropped on the Pi, not here: with a terminal, sudo's password
    # prompt comes back on ssh's standard output, and a redirection here would hide it.
    # The time the password takes is added: the Pi's own clock has counted it since
    # $theirs was read.
    # shellcheck disable=SC2016
    remote_sudo sh -c 'date -u -s "@$(( $1 + $(date -u +%s) - $2 ))" > /dev/null' sh "$ours" "$theirs"
}

# ---- the laptop's video --------------------------------------------------------------

alive() { # pid-file name: 0 if that process runs and is called name
    local pid; [ -f "$1" ] || return 1
    pid=$(cat "$1"); [ -n "$pid" ] && [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "$2" ]
}

stop_pidfile() { # pid-file name
    local pid
    alive "$1" "$2" || { rm -f "$1"; return 0; }
    pid=$(cat "$1")
    kill -TERM "$pid"
    for _ in $(seq 1 30); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill -0 "$pid" 2>/dev/null && kill -KILL "$pid"
    rm -f "$1"
}

# hellion and eeye in placement A (profiles/rov-laptop): the camera's H.264 from 5610,
# the effects here, QGroundControl on 127.0.0.1:5600. Each in its own session, its pid
# in a file: never a pattern to match, never a wait.
laptop_video_up() {
    [ -x "$HELLION" ] && [ -x "$EEYE" ] || { warn "no $HELLION or $EEYE: run make -j8 all in $repo"; return 1; }
    alive "$LAB_STATE_DIR/hellion.pid" hellion && { say "the laptop's hellion already runs"; return 0; }
    sed -e "s|@CONTROL_PORT@|$HELLION_CONTROL_PORT|" -e "s|@EEYE_CONFIG@|$LAB_STATE_DIR/eeye.json|" \
        "$repo/profiles/rov-laptop/hellion-a-topside.json" > "$LAB_STATE_DIR/hellion.json"
    [ -f "$LAB_STATE_DIR/eeye.json" ] || cp "$repo/profiles/rov-laptop/eeye-topside.json" "$LAB_STATE_DIR/eeye.json"
    (cd "$LAB_STATE_DIR" && exec setsid "$HELLION" -c "$LAB_STATE_DIR/hellion.json" > hellion.log 2>&1 < /dev/null) &
    echo $! > "$LAB_STATE_DIR/hellion.pid"
    sleep 2
    (cd "$LAB_STATE_DIR" && exec setsid "$EEYE" "$LAB_STATE_DIR/eeye.json" > eeye.log 2>&1 < /dev/null) &
    echo $! > "$LAB_STATE_DIR/eeye.pid"
    disown -a
    sess_set laptop_video 1
}

laptop_video_down() {
    stop_pidfile "$LAB_STATE_DIR/eeye.pid" eeye
    stop_pidfile "$LAB_STATE_DIR/hellion.pid" hellion
}

# The pipelines of THIS session's hellion, from its own page (its port): `hellion check`
# reads the status file of whatever hellion is named "topside" on this machine, which is
# the laptop's own hellion.service when that runs. Returns 1 if an endpoint is in error.
laptop_pipelines() {
    curl -fsS -m 4 "http://127.0.0.1:$HELLION_CONTROL_PORT/api/status" 2>/dev/null | python3 -I -c '
import json, sys
try:
    d = json.load(sys.stdin)
except ValueError:
    print("  hellion does not answer on its port"); sys.exit(1)
bad = 0
for p in d.get("pipelines", []):
    for e in p.get("endpoints", []):
        err = e.get("state") == "error"
        bad += err
        print("  [%s] pipeline %s: %s %s: %s, %d frames, %.0f frames/s%s" % (
            "FAIL" if err else " OK ", p["name"], e["role"], e["spec"], e["state"], e.get("frames", 0),
            e.get("fps", 0), ("; " + e["detail"]) if e.get("detail") else ""))
sys.exit(1 if bad else 0)'
}

# What holds a UDP port on this laptop, in one line; nothing printed if it is free.
udp_holder() { ss -Hlnup "( sport = :$1 )" 2>/dev/null | head -1; }

# ---- preflight -----------------------------------------------------------------------

# tools/rov/preflight.py's probe on the Pi (the file on python3's standard input: the
# installed release need not have it) and on this laptop, then its judge over both.
# Rehearsing, the two are one machine: one probe, with both machines' ports.
# preflight_run MODE VIDEO BRIDGE SERIAL: the report on standard output, 1 at a FAIL.
PF=$here/preflight.py
preflight_run() {
    local mode=$1 video=$2 bridge=$3 serial=$4 tmp rc pi_at own=""
    local pp="" lp=""
    if [ "$mode" = up ]; then
        # What the session binds. The Pi: the vehicle's console and mavlink module, the
        # relay unless --serial, and the topside role or the bridge. This laptop: with
        # --bridge the topside role and its bridge, with --video the camera's stream and
        # hellion's page, and the consoles' forwards.
        pp="tcp:8080,udp:14560"
        [ "$RELAY" = 1 ] && [ "$serial" != 1 ] && pp+=",udp:14555"
        if [ "$bridge" = 1 ]; then pp+=",udp:$V_PORT"; lp+=",udp:$T_PORT,tcp:8081,udp:14650"
        else pp+=",tcp:8081,udp:14650"; fi
        [ "$video" = 1 ] && lp+=",udp:5610,tcp:$HELLION_CONTROL_PORT"
        if ! is_local; then
            lp+=",tcp:$CONSOLE_VEHICLE_PORT"
            [ "$bridge" = 1 ] || lp+=",tcp:$CONSOLE_TOPSIDE_PORT"
        fi
        lp=${lp#,}
    fi
    local p
    for p in "$LAB_STATE_DIR/hellion.pid" "$LAB_STATE_DIR/eeye.pid"; do
        [ -f "$p" ] && own+="$(cat "$p"),"
    done
    tmp=$(mktemp -d)
    local pargs=(probe --services "$LAB_SERVICES $LAB_SERIAL_SERVICES" --root "$ROV_ROOT" --hangar-root "$RV,$RT"
                 --own-pidfiles hangar-rov/relay.pid --frozen-file "$FROZEN_FILE")
    if is_local; then
        python3 -I - "${pargs[@]}" --ports "$pp${lp:+,$lp}" < "$PF" > "$tmp/pi.json" 2> "$tmp/pi.err"
        pi_at=$(date +%s.%N)
        python3 -I "$PF" judge --mode "$mode" --pi "$tmp/pi.json" --own-pids "$own"
        rc=$?
    else
        ssh "${SSH_OPTS[@]}" "$TARGET" "python3 -I - $(printf '%q ' "${pargs[@]}" --ports "$pp")" < "$PF" \
            > "$tmp/pi.json" 2> "$tmp/pi.err"
        pi_at=$(date +%s.%N)
        python3 -I "$PF" probe --ports "$lp" --services hellion --hangar-root "$RT" > "$tmp/laptop.json" 2> "$tmp/laptop.err"
        python3 -I "$PF" judge --mode "$mode" --pi "$tmp/pi.json" --laptop "$tmp/laptop.json" --pi-read-at "$pi_at" \
            --own-pids "$own"
        rc=$?
    fi
    rm -rf "$tmp"
    return "$rc"
}

cmd_preflight() {
    local mode=up video=0 bridge=0 serial=0 a
    # An open session's own choices, unless the command line says.
    [ "$(sess_get laptop_video)" = 1 ] && video=1
    [ "$(sess_get bridge)" = 1 ] && bridge=1
    [ "$(sess_get serial)" = 1 ] && serial=1
    for a in "$@"; do
        case $a in
            --measure) mode=measure ;;
            --video) video=1 ;;
            --bridge) bridge=1 ;;
            --serial) serial=1 ;;
            *) usage 2 ;;
        esac
    done
    reachable || exit 1
    preflight_run "$mode" "$video" "$bridge" "$serial"
}

# ---- the consoles --------------------------------------------------------------------

# Dawn Patrol listens on the Pi's loopback only: two forwards on the SSH connection.
# With --bridge the topside's console is on this laptop itself: the vehicle's alone.
tunnels_up() { # bridge
    is_local && return 0
    local fwd=(-L "$CONSOLE_VEHICLE_PORT:127.0.0.1:8080")
    [ "${1:-0}" = 1 ] || fwd+=(-L "$CONSOLE_TOPSIDE_PORT:127.0.0.1:8081")
    tunnels_down
    ssh "${SSH_OPTS[@]}" -O forward "${fwd[@]}" "$TARGET" 2>/dev/null &&
        sess_set tunnels 1
}
tunnels_down() { # each by itself: one that is not there does not keep the other
    is_local && return 0
    ssh "${SSH_OPTS[@]}" -O cancel -L "$CONSOLE_VEHICLE_PORT:127.0.0.1:8080" "$TARGET" 2>/dev/null
    ssh "${SSH_OPTS[@]}" -O cancel -L "$CONSOLE_TOPSIDE_PORT:127.0.0.1:8081" "$TARGET" 2>/dev/null
    return 0
}

# ---- deploy --------------------------------------------------------------------------

cmd_deploy() {
    local realtime=0
    if [ "${1:-}" = --realtime ]; then realtime=1; shift; fi
    local tb=${1:-}
    if [ -z "$tb" ]; then
        local arch=aarch64; is_local && arch=$(uname -m)
        tb=$(ls -t "$repo"/out/release/*/hangar-*-"$arch".tar.zst "$repo"/out/release/hangar-*-"$arch".tar.zst 2>/dev/null | head -1)
        [ -n "$tb" ] || die "no hangar-*-$arch.tar.zst under $repo/out/release: tools/release.sh, or give one"
    fi
    [ -f "$tb" ] || die "no file $tb"
    reachable || exit 1
    hangar_running && die "hangar runs on the Pi: lab.sh down first (the binaries are in use)"
    set_clock || die "could not set the Pi's clock"
    local name; name=$(basename "$tb" .tar.zst)
    say "copying $name to the Pi"
    remote <<< 'mkdir -p "$HOME/hangar-visit"' && copy_to "$tb" hangar-visit || die "copy failed"
    remote NAME="$name" <<'EOF' || die "install failed: nothing of the running system was touched"
set -e
case $ROOT in /*) R=$ROOT ;; *) R=$HOME/$ROOT ;; esac
cd "$HOME/hangar-visit"
rm -rf "$NAME"
tar --zstd -xf "$NAME.tar.zst"
cd "$NAME"
grep -E '^[0-9a-f]{64}  ' MANIFEST | sha256sum -c --quiet || { echo "the files do not match the MANIFEST's hashes"; exit 3; }
sh share/hangar/install-root.sh "$R" > /dev/null
cp MANIFEST "$R/MANIFEST"    # which release this install is: up --bridge compares its commit
echo "lab: installed $(sed -n 's/^version //p' MANIFEST) ($(sed -n 's/^commit //p' MANIFEST | cut -c1-7)) at $R"
EOF
    [ "$realtime" = 1 ] && deploy_realtime
    return 0
}

# deploy --realtime (docs/13, "Real-time scheduling without root"): the two capabilities
# on the installed hangard, by which a manager that is not root gives a module the
# policy, the priority and the locked memory its [Schedule] asks. Once per install: the
# kernel drops a file's capabilities when the file is written again, so every deploy
# that is to have them asks again. The file is closed to other users first: whoever can
# run it can have a process at a real-time priority.
deploy_realtime() {
    local bin opts
    bin=$(rsh 'echo "$B/hangard"') || die "could not find the installed hangard"
    opts=$(rsh 'findmnt -no OPTIONS -T "$B/hangard" 2>/dev/null')
    case ",$opts," in
        *,nosuid,*) warn "the filesystem of $bin is mounted nosuid: the kernel ignores file capabilities there."
                    warn "  Install under another filesystem, or grant by limits (docs/13). Nothing was changed"; return 1 ;;
    esac
    rsh 'chmod 750 "$B/hangard"' || die "could not close $bin to other users"
    say "sudo on the Pi, for: setcap cap_sys_nice,cap_ipc_lock+ep $bin"
    remote_sudo setcap cap_sys_nice,cap_ipc_lock+ep "$bin" || { warn "setcap failed: real time stays out of reach (lab.sh preflight says)"; return 1; }
    is_local || say "now: $(rsh 'getcap "$B/hangard" 2>/dev/null')"
    say "a module gets real time only if its manifest has a [Schedule]; hangar check says what is in force"
}

# ---- up ------------------------------------------------------------------------------

# The vehicle's status line of a module ("" if none).
status_of() { rsh MOD="$1" '"$B/hangar" status 2>/dev/null | awk -v m="$MOD" '\''$1 == m { $1 = $1; print }'\'''; }

wait_for() { # module regex seconds: until the module's status line matches
    local t=0 line
    while [ "$t" -lt "$3" ]; do
        line=$(status_of "$1")
        [[ $line =~ $2 ]] && return 0
        sleep 2; t=$((t + 2))
    done
    warn "$1, after $3 s: ${line:-no status}"
    return 1
}

up_failed() {
    warn "up stopped here. Nothing is lost: lab.sh status shows what runs, lab.sh down puts everything back."
    exit 1
}

cmd_up() {
    local video=0 battery=1 bridge=0 serial=0 pixhawk=0 preflight=1 out rc
    for a in "$@"; do
        case $a in
            --video) video=1 ;;
            --bridge) bridge=1 ;;
            --serial) serial=1 ;;
            --no-battery-check) battery=0 ;;
            --no-preflight) preflight=0 ;;
            --realtime) LAB_REALTIME=1 ;;
            *) usage 2 ;;
        esac
    done
    reachable || exit 1
    rsh '[ -x "$ROV" ] && [ -x "$B/hangard" ]' || die "no hangar at $ROV_ROOT on the Pi: lab.sh deploy first"
    # up lays hangar out afresh: it stops what runs. Never under an armed vehicle, and
    # up disarms nothing: asked before anything is changed, here or on the Pi.
    if hangar_running; then
        vehicle_state
        case $VSTATE in
            disarmed) ;;
            armed)
                warn "THE VEHICLE IS NOT DISARMED ($VWHY)."
                warn "  up would stop hangar to lay it out again. Disarm first (QGroundControl, the console),"
                die "  then up again. Nothing was changed" ;;
            *)
                warn "hangar runs on the Pi and whether the vehicle is disarmed cannot be read:"
                warn "  $VWHY"
                warn "  up would stop it. lab.sh status shows what runs; lab.sh down --force brings it down as it is."
                die "  Nothing was changed" ;;
        esac
    fi
    # The preflight, before anything is changed: a FAIL is something that would keep the
    # session from coming up as meant (a port taken, no room), and up stops at it.
    if [ "$preflight" = 1 ]; then
        say "preflight (lab.sh preflight shows it again; up --no-preflight goes on past a FAIL)"
        preflight_run up "$video" "$bridge" "$serial" ||
            die "the preflight has a FAIL: put it right, or up --no-preflight. Nothing was changed"
    fi
    if [ "$video" = 1 ]; then
        [ -x "$HELLION" ] && [ -x "$EEYE" ] || die "no $HELLION or $EEYE: run make -j8 all in $repo"
        ss -lnt "( sport = :$HELLION_CONTROL_PORT )" 2>/dev/null | grep -q LISTEN &&
            ! alive "$LAB_STATE_DIR/hellion.pid" hellion &&
            die "port $HELLION_CONTROL_PORT is taken on this laptop: set HELLION_CONTROL_PORT"
        # The camera's H.264 arrives on UDP 5610. Another hellion holding it (this laptop's
        # own hellion.service does) leaves ours without a picture and eeye without input,
        # while the pilot still sees video: the other one's, which no control here changes.
        if ! alive "$LAB_STATE_DIR/hellion.pid" hellion && [ -n "$(udp_holder 5610)" ]; then
            warn "UDP port 5610 (the camera's video) is taken on this laptop:"
            warn "  $(udp_holder 5610 | awk '{print $NF}')"
            systemctl is-active --quiet hellion 2>/dev/null &&
                warn "  this laptop's hellion.service is running: sudo systemctl stop hellion"
            die "--video needs port 5610 free"
        fi
    fi
    # The Pixhawk is reached through the relay, or with --serial by the module itself.
    if [ "$RELAY" = 1 ] || [ "$serial" = 1 ]; then pixhawk=1; fi
    if [ "$bridge" = 1 ]; then
        local b
        for b in hangard hangar hangar-mavlink hangar-bridge dawn-patrold; do
            [ -x "$repo/builddir/$b" ] || die "--bridge: no $repo/builddir/$b for the topside role here: make -j8 all in $repo"
        done
        rsh '[ -x "$B/hangar-bridge" ] && grep -q BRIDGE_PEER_PORT "$ROV"' ||
            die "--bridge: the Pi's release has no bridge, or a rov.sh older than this lab.sh: lab.sh deploy a release of this checkout"
        same_build || exit 1
    fi
    if [ "$serial" = 1 ]; then
        rsh 'grep -q MAVLINK "$ROV"' ||
            die "--serial: the Pi's rov.sh does not know MAVLINK=serial: lab.sh deploy a release of this checkout"
        # Asked before anything is changed; and again below, once what up stops is stopped.
        out=$(serial_holders 1); rc=$?
        if [ "$rc" != 0 ]; then serial_refused "$rc" "$out"; die "nothing was changed"; fi
    fi
    # A session this laptop has lost and the Pi still knows of is the one to continue:
    # a new one would save hangar's own values as "the parameters as found".
    [ -f "$SESSION" ] || { sess_pull && say "no session file on this laptop: taken from the Pi's copy"; }
    [ -f "$SESSION" ] && say "a session is open since $(sess_get started): continuing it"
    [ -f "$SESSION" ] || sess_set started "$(date -Is)"
    set_clock || up_failed
    # What this up lays out, for status and for a down that has to start hangar again.
    sess_set bridge "$bridge"
    sess_set serial "$serial"
    sess_set realtime "${LAB_REALTIME:-0}"

    # A topside left on this laptop by an earlier up of the session: stopped, to be laid
    # out afresh or not at all.
    if [ "$(sess_get topside)" = 1 ] || topside_laid_out; then
        topside reset > /dev/null || up_failed
        sess_set topside 0
    fi

    # Whatever of the lab's holds the Pixhawk's serial line (LAB_SERIAL_SERVICES).
    if [ "$serial" = 1 ] && [ -n "$LAB_SERIAL_SERVICES" ]; then
        say "stopping the lab's $LAB_SERIAL_SERVICES on the Pi (started again by down)"
        # shellcheck disable=SC2086
        stop_services $LAB_SERIAL_SERVICES || up_failed
    fi

    # The lab's electric-eye holds the camera and would send to the laptop's 5610 too.
    if [ "$video" = 1 ]; then
        say "stopping the lab's $LAB_SERVICES on the Pi (started again by down)"
        # shellcheck disable=SC2086
        stop_services $LAB_SERVICES || up_failed
    fi

    # hangar, laid out afresh so that the encoder is in it or not as asked.
    local source=$H264_NODE
    [[ $source == *:* ]] || source=uvc:$source
    local what="hangar up on the Pi"
    [ "$bridge" = 1 ] && what="the vehicle role up on the Pi, with the bridge"
    [ "$serial" = 1 ] && what="$what, the mavlink module on ${FCU%%:*} itself"
    [ "$video" = 1 ] && what="$what, the encoder sending $source"
    say "$what"
    rsh 'mkdir -p "$HOME/hangar-rov"; "$ROV" reset > /dev/null' || up_failed
    if [ "$serial" = 1 ]; then
        out=$(serial_holders 0); rc=$?
        if [ "$rc" != 0 ]; then serial_refused "$rc" "$out"; up_failed; fi
    fi
    rov_env "$bridge" "$serial"
    rsh ENCODER="$video" ENCODER_SOURCE="$source" "${ROV_ENV[@]}" \
        '"$ROV" up > "$HOME/hangar-rov/up.log" 2>&1; rc=$?
         grep -E "^rov: (relay|hangar is up|the vehicle role is up|RELAY|[^ ]* was laid out|no |missing |MAVLINK)" "$HOME/hangar-rov/up.log"; exit $rc' || up_failed
    sess_set hangar_up 1
    wait_for safety 'ready' 30 || up_failed

    # The topside role here, and the link: before the parameters, so that a link that
    # does not come up, or two builds that do not match, stop up with nothing written
    # to the Pixhawk.
    if [ "$bridge" = 1 ]; then
        local state=${HANGAR_ROV_STATE:-$HOME/hangar-rov}
        if is_local; then
            say "rehearsal: both roles read the one $state/bridge.key"
        else
            say "the bridge's key from the Pi to $state/bridge.key (mode 600; it stays there, as the Pi's stays on the Pi)"
            (umask 077; mkdir -p "$state") && rm -f "$state/bridge.key" &&
                (umask 077; scp -q "${SSH_OPTS[@]}" "$TARGET:hangar-rov/bridge.key" "$state/bridge.key") &&
                chmod 600 "$state/bridge.key" || { warn "the key could not be copied"; up_failed; }
        fi
        say "the topside role up on this laptop ($RT)"
        sess_set topside 1
        sess_push
        topside up > "$LAB_STATE_DIR/topside-up.log" 2>&1 ||
            { sed 's/^/  /' "$LAB_STATE_DIR/topside-up.log" >&2; up_failed; }
        wait_link || up_failed
    fi

    if [ "$pixhawk" = 0 ]; then
        say "RELAY=0: no Pixhawk; ArduSub's parameters are not touched"
    else
        wait_for mavlink 'ready .*ardusub (disarmed|armed)' 60 || up_failed
        # The backup of the parameters as the lab left them: once per session, so a
        # second up never saves hangar's own values over it. Through hangar (rov.sh
        # params, hangar ardusub-params), which is up by now: NAME VALUE TYPE lines.
        local backup; backup=$(sess_get params_backup)
        if [ -z "$backup" ]; then
            backup=hangar-rov/lab-$(date +%Y%m%d-%H%M%S)-before.params
            say "saving ArduSub's parameters to the Pi's ~/$backup"
            rsh FILE="$backup" '"$ROV" params save "$HOME/$FILE"' ||
                { warn "the parameters could not be saved: nothing is written to the Pixhawk without a backup"; up_failed; }
            sess_set params_backup "$backup"
            sess_push
        else
            say "the session's parameter backup is ~/$backup"
        fi
        say "setting hangar's parameters on ArduSub (hangar ardusub-setup)"
        rsh 'timeout 60 "$B/hangar" ardusub-setup > /dev/null' || up_failed
        wait_for mavlink 'params 1' 30 || warn "not every parameter reads as hangar's file yet: see lab.sh status"
    fi

    if [ "$battery" = 0 ]; then
        say "the battery check OFF for this session (the Pixhawk reads no voltage on the lab's vehicle)"
        rsh '"$B/hangar" param set safety battery_min=0 battery_required=0 > /dev/null' || up_failed
        sess_set battery_check off
    fi

    tunnels_up "$bridge" || warn "the consoles' tunnels did not come up (ports $CONSOLE_VEHICLE_PORT, $CONSOLE_TOPSIDE_PORT taken?)"
    if [ "$video" = 1 ]; then
        laptop_video_up || up_failed
        sleep 6
        laptop_pipelines ||
            warn "THE VIDEO IS NOT WORKING on this laptop (above; $LAB_STATE_DIR/hellion.log): the vehicle is not affected"
    fi

    local check
    check=$(rsh 'timeout 30 "$B/hangar" check 2>&1 | grep -E "^(ok|detail|OK schedule|WARN schedule)"')
    echo
    # What each [Schedule] asks against what is in force: said whenever one is not,
    # and with --realtime either way.
    if grep -q '^WARN schedule' <<< "$check" || [ "${LAB_REALTIME:-0}" = 1 ]; then
        grep -E '^(OK|WARN) schedule' <<< "$check" | sed 's/^/  /'
    fi
    if [[ $check =~ ok\ +1 ]]; then
        say "READY TO ARM: hangar check passes"
    else
        say "NOT READY TO ARM: $(sed -n 's/^detail *//p' <<< "$check" | sed 's/ *#.*//')"
    fi
    if is_local; then
        echo "  consoles   http://127.0.0.1:8080 (vehicle), http://127.0.0.1:8081 (topside)"
    elif [ "$bridge" = 1 ]; then
        echo "  consoles   http://127.0.0.1:$CONSOLE_VEHICLE_PORT (vehicle, tunnelled), http://127.0.0.1:8081 (topside, on this laptop)"
    else
        echo "  consoles   http://127.0.0.1:$CONSOLE_VEHICLE_PORT (vehicle), http://127.0.0.1:$CONSOLE_TOPSIDE_PORT (topside)"
    fi
    [ "$bridge" = 1 ] && echo "  bridge     the topside runs on this laptop; the tether trigger is ON: arming needs the link up"
    [ "$serial" = 1 ] && echo "  MAVLink    the module holds ${FCU%%:*} itself: no MAVROS relay runs"
    [ "$video" = 1 ] && echo "  video      QGroundControl on UDP 5600; the effects on http://127.0.0.1:$HELLION_CONTROL_PORT"
    echo "  QGC        connects by itself (UDP 14550): arm there, drive with the sticks"
    echo "  then       lab.sh status, lab.sh bench, and lab.sh down when done"
}

# ---- status --------------------------------------------------------------------------

cmd_status() {
    reachable || exit 1
    # With the bridge the Pi has the vehicle's root alone; the topside's is this laptop's.
    local roots="$RV $RT" bridge=0 serial_dev=""
    if bridge_session; then roots=$RV; bridge=1; fi
    [ "$(sess_get serial)" = 1 ] && serial_dev=${FCU%%:*}
    echo "== the Pi ($TARGET)"
    remote SERVICES="$LAB_SERVICES $LAB_SERIAL_SERVICES" ROOTS="$roots" SERIAL_DEV="$serial_dev" <<'EOF'
case $ROOT in /*) R=$ROOT ;; *) R=$HOME/$ROOT ;; esac
B=$R/bin
for root in $ROOTS; do
    if [ -f "$root/hangard.pid" ] && kill -0 "$(cat "$root/hangard.pid")" 2>/dev/null; then
        HANGAR_ROOT=$root "$B/hangar" status 2>/dev/null | grep -vE '^MODULE' | cut -c1-120 | sed "s|^|  |"
    else
        echo "  no manager under $root"
    fi
done
# rov.sh's word for the relay: a relay.pid left from before a reboot names no relay.
echo "  MAVROS relay: $("$R/share/hangar/rov.sh" relay status 2>/dev/null || echo "not known")${SERIAL_DEV:+ (--serial: the mavlink module holds $SERIAL_DEV itself)}"
for s in $SERVICES; do printf '  %s: %s\n' "$s" "$(systemctl is-active "$s" 2>/dev/null)"; done
if command -v vcgencmd > /dev/null; then
    echo "  $(vcgencmd measure_temp) $(vcgencmd get_throttled) $(vcgencmd measure_clock arm | sed 's/frequency([0-9]*)=/arm Hz=/')"
fi
echo "  clock $(date -u '+%F %T') UTC, up $(uptime -p | sed 's/^up //')"
EOF
    echo "== this laptop"
    if [ "$bridge" = 1 ]; then
        if [ -f "$RT/hangard.pid" ] && kill -0 "$(cat "$RT/hangard.pid")" 2>/dev/null; then
            topside_hangar status 2>/dev/null | grep -vE '^MODULE' | cut -c1-120 | sed "s|^|  |"
            echo "  topside console http://127.0.0.1:8081"
        else
            echo "  no manager under $RT (the topside role: the link is down, and nothing arms)"
        fi
    fi
    if alive "$LAB_STATE_DIR/hellion.pid" hellion; then
        echo "  hellion pid $(cat "$LAB_STATE_DIR/hellion.pid"), page http://127.0.0.1:$HELLION_CONTROL_PORT"
        laptop_pipelines
    else
        echo "  hellion: not running"
    fi
    alive "$LAB_STATE_DIR/eeye.pid" eeye && echo "  eeye pid $(cat "$LAB_STATE_DIR/eeye.pid")" || echo "  eeye: not running"
    if ! is_local && ssh "${SSH_OPTS[@]}" -O check "$TARGET" 2>/dev/null; then
        [ "$(sess_get tunnels)" = 1 ] && echo "  consoles http://127.0.0.1:$CONSOLE_VEHICLE_PORT, :$CONSOLE_TOPSIDE_PORT"
    fi
    echo "== the session"
    if [ -f "$SESSION" ]; then sed 's/^/  /' "$SESSION"; else echo "  none open"; fi
    frozen_notice
    return 0
}

# ---- bench ---------------------------------------------------------------------------

# The camera's controls: the parameters of the vehicle's encoder (msg/encoder_params.msg).
# Set on the Pi itself: that needs neither the bridge nor a console, and the refusal of a
# value comes back as hangar words it.
cmd_camera() {
    reachable || exit 1
    rsh '"$B/hangar" status 2>/dev/null | grep -q "^encoder "' ||
        die "no encoder runs on the Pi: the camera's controls come with lab.sh up --video"
    local a
    for a in "$@"; do
        [[ $a =~ ^[a-z_]+=-?[0-9]+$ ]] || die "camera: '$a' is not field=number (lab.sh camera alone lists the fields)"
    done
    if [ $# -gt 0 ]; then
        rsh SET="$*" '"$B/hangar" param set encoder $SET' || exit 1
    fi
    rsh '"$B/hangar" param get encoder; "$B/hangar" status | awk "\$1 == \"encoder\"" | grep -o "camera controls[^;]*" || true'
}

cmd_bench() {
    local secs=${1:-60}
    [[ $secs =~ ^[0-9]+$ ]] || usage 2
    reachable || exit 1
    local dir; dir=$RESULTS_DIR/$(date +%Y-%m-%d)-lab
    local n=2; while [ -e "$dir" ]; do dir=$RESULTS_DIR/$(date +%Y-%m-%d)-lab-$n; n=$((n + 1)); done
    mkdir -p "$dir"
    say "benchmarking both machines for $secs s into $dir"
    remote <<< 'mkdir -p "$HOME/hangar-rov"' &&
        copy_to "$repo/bench/procbench.py" hangar-rov && copy_to "$here/fpswatch.sh" hangar-rov || die "copy failed"
    local riface liface
    riface=$(remote IP="$ROV_IP" <<< 'ip -o -4 addr | awk -v ip="$IP" '"'"'index($4, ip "/") == 1 { print $2; exit }'"'"'')
    liface=$(ip route get "$ROV_IP" 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -1)
    cmd_status > "$dir/state-before.txt" 2>&1
    # What else the two machines are doing, before and after: a run with a FAIL in
    # either is marked in its own folder (NOT-CLEAN and the README's first lines).
    local clean=1
    preflight_run measure 0 0 0 > "$dir/preflight-before.txt" 2>&1 || clean=0
    grep -E '^  (warn|FAIL)' -A1 "$dir/preflight-before.txt" | grep -v '^--$'
    [ "$clean" = 1 ] || warn "the preflight has a FAIL: this run is marked NOT CLEAN (it runs all the same)"
    # With the bridge: its counters on both ends, before and after (rtt, loss, rejected).
    local bridge=0
    bridge_session && bridge=1
    link_counters() { # file-suffix
        rsh '"$B/hangar" echo bridge.vehicle.link --count 1 2>&1' > "$dir/rov-link-$1.txt"
        topside_hangar echo bridge.topside.link --count 1 > "$dir/laptop-link-$1.txt" 2>&1
    }
    [ "$bridge" = 1 ] && link_counters before

    # The laptop's two samplers in the background, waited on by their pids alone.
    python3 -I "$repo/bench/procbench.py" "$secs" > "$dir/laptop-procbench.txt" 2>&1 &
    local p1=$!
    local fargs=(-s "$secs")
    alive "$LAB_STATE_DIR/hellion.pid" hellion && fargs+=(-H "$HELLION")
    [ -n "$liface" ] && fargs+=(-i "$liface")
    bash "$here/fpswatch.sh" "${fargs[@]}" > "$dir/laptop-fps.txt" 2>&1 &
    local p2=$!

    # On the Pi: the same, with the encoder's counters and the lab's hellion if it runs.
    remote SECS="$secs" IFACE="$riface" <<'EOF'
case $ROOT in /*) R=$ROOT ;; *) R=$HOME/$ROOT ;; esac
cd "$HOME/hangar-rov"
args=(-s "$SECS" -e "$R/bin/hangar")
[ -n "$IFACE" ] && args+=(-i "$IFACE")
command -v hellion > /dev/null && systemctl is-active --quiet hellion 2>/dev/null && args+=(-H "$(command -v hellion)")
bash ./fpswatch.sh "${args[@]}" > rov-fps.txt 2>&1 &
f=$!
# PSS of other users' processes needs root: sudo without a password if allowed, else
# the memory of those reads "-".
if sudo -n true 2>/dev/null; then sudo -n python3 -I ./procbench.py "$SECS" > rov-procbench.txt 2>&1
else python3 -I ./procbench.py "$SECS" > rov-procbench.txt 2>&1; fi
wait "$f"
EOF
    wait "$p1" "$p2"
    copy_from hangar-rov/rov-procbench.txt "$dir" && copy_from hangar-rov/rov-fps.txt "$dir" || warn "could not copy the Pi's results"
    cmd_status > "$dir/state-after.txt" 2>&1
    preflight_run measure 0 0 0 > "$dir/preflight-after.txt" 2>&1 || clean=0
    [ "$bridge" = 1 ] && link_counters after
    [ "$clean" = 1 ] || grep -h '^  FAIL' -A1 "$dir/preflight-before.txt" "$dir/preflight-after.txt" > "$dir/NOT-CLEAN"
    {
        echo "# Bench, $(date -Is)"
        echo
        if [ "$clean" = 1 ]; then
            echo "Preflight: no FAIL before or after (preflight-before.txt, preflight-after.txt)."
        else
            echo "**NOT CLEAN.** The preflight found, before or after, what spoils a number"
            echo "(preflight-before.txt, preflight-after.txt):"
            echo; echo '```'; cat "$dir/NOT-CLEAN"; echo '```'
        fi
        echo
        echo "$secs s on both machines, by tools/rov/lab.sh bench (hangar $(git -C "$repo" describe --always 2>/dev/null))."
        echo "The state before and after: state-before.txt, state-after.txt (armed or not, temperature,"
        echo "throttling: a throttled or hot Pi reads high). Frame rates: hellion's \"frames in ~1.2 s\""
        echo "reads ~32 for 30 fps; the encoder's own counter is exact."
        echo
        local files="rov-procbench rov-fps laptop-procbench laptop-fps"
        if [ "$bridge" = 1 ]; then
            echo "The bridge setup: the vehicle role on the Pi (rov-procbench), the topside role on this"
            echo "laptop (laptop-procbench's hangar lines); the two bridges' link topics before and after"
            echo "(rtt, loss, retransmits, rejected: the differences are the window's)."
            echo
            files="$files rov-link-before rov-link-after laptop-link-before laptop-link-after"
        fi
        for f in $files; do
            echo "## $f"; echo; echo '```'; cat "$dir/$f.txt" 2>/dev/null; echo '```'; echo
        done
    } > "$dir/README.md"
    sed -n '/^## rov-fps/,$p' "$dir/README.md" | grep -E '^  |^## ' ; grep -E 'whole machine|  total' "$dir/rov-procbench.txt" "$dir/laptop-procbench.txt" 2>/dev/null | sed "s|$dir/||"
    if [ "$bridge" = 1 ]; then
        echo "  the link, from the Pi:      $(status_of bridge | cut -c1-110)"
        echo "  the link, from this laptop: $(topside_line bridge | cut -c1-110)"
    fi
    say "saved in $dir"
}

# ---- down ----------------------------------------------------------------------------

# hangar on the Pi again, as up started it but without the encoder, until the mavlink
# module hears ArduSub: for a restore after hangar stopped.
hangar_up_again() {
    local bridge=0 serial=0
    [ "$(sess_get bridge)" = 1 ] && bridge=1
    [ "$(sess_get serial)" = 1 ] && serial=1
    [ "$(sess_get realtime)" = 1 ] && LAB_REALTIME=1
    rov_env "$bridge" "$serial"
    rsh "${ROV_ENV[@]}" \
        'mkdir -p "$HOME/hangar-rov"; "$ROV" up > "$HOME/hangar-rov/up.log" 2>&1' || return 1
    wait_for mavlink 'ready .*ardusub (disarmed|armed)' 60
}

# The end of a down that found no session file, here or on the Pi. If nothing of hangar
# ran there either, there was nothing to put back. If hangar ran, up had been at work
# and what it changed is unknown: "as found" is not claimed, and what can be seen is said.
down_without_session() { # hangar-was-running failed-steps
    local inactive=""
    is_local || inactive=$(remote SERVICES="$LAB_SERVICES" <<< \
        'for s in $SERVICES; do systemctl is-active --quiet "$s" || printf "%s " "$s"; done')
    is_local || ssh "${SSH_OPTS[@]}" -O exit "$TARGET" 2>/dev/null
    if [ "$1" = 0 ] && [ "$2" = 0 ]; then
        say "no session was open and nothing of hangar ran on the Pi: nothing to put back"
        [ -z "$inactive" ] || say "  (not running on the Pi: $inactive)"
        return 0
    fi
    warn "NO SESSION FILE ($SESSION), here or on the Pi: what lab.sh up changed is not known."
    [ "$1" = 1 ] && warn "  hangar ran on the Pi and is stopped now."
    warn "  ArduSub's parameters were NOT restored. If up saved them, the backup is the Pi's"
    warn "  newest ~/hangar-rov/lab-*-before.params; on the Pi: rov.sh up, rov.sh params restore"
    warn "  <that file>, rov.sh reset (tools/rov/README.md, \"The parameter backup\")."
    [ -z "$inactive" ] || warn "  Not running on the Pi: $inactive(if up --video stopped them: sudo systemctl start $inactive)"
    warn "not claimed: everything as found"
    exit 1
}

cmd_down() {
    local failed=() was_up=0 lost=0 force=0
    for a in "$@"; do
        case $a in --force) force=1 ;; *) usage 2 ;; esac
    done
    step() { # what command...: never stops at a failure, says which ones failed
        local what=$1; shift
        if "$@"; then say "ok    $what"; else warn "FAILED $what"; failed+=("$what"); fi
    }
    if ! reachable; then
        step "the laptop's hellion and eeye stopped" laptop_video_down
        warn "the Pi is out of reach: its side is left as it is; the session file is kept for the next down"
        topside_laid_out && warn "the topside role on this laptop is left running too: it is the vehicle's link when the tether is back"
        exit 1
    fi

    # Before anything is stopped, here or there: the vehicle disarmed. down disarms it
    # itself if it is not (it is the session's end, and safety.disarm is hangar's own
    # ordinary way), and then reads the state again. Still armed, or not readable: down
    # stops nothing, not even the pilot's video, unless told to with --force. The Pi's
    # rov.sh asks the same again when it comes to stopping (rov.sh reset, below).
    if hangar_running; then
        was_up=1
        vehicle_state
        if [ "$VSTATE" != disarmed ]; then
            if [ "$VSTATE" = armed ]; then warn "the vehicle was found NOT DISARMED ($VWHY): disarming it"
            else warn "the vehicle's state cannot be read ($VWHY): asking for a disarm"; fi
            # The three seconds are ArduSub's, to report itself disarmed.
            rsh '"$B/hangar" call safety.disarm > /dev/null; sleep 3'
            vehicle_state
        fi
        if [ "$VSTATE" = disarmed ]; then
            say "ok    disarmed ($VWHY)"
        elif [ "$force" = 1 ]; then
            warn "--force: hangar is stopped although the vehicle does not read disarmed ($VWHY)"
        else
            warn "THE VEHICLE DOES NOT READ DISARMED: $VWHY"
            warn "  Nothing was stopped, on the Pi or on this laptop; the session is as it was."
            warn "  Disarm it (QGroundControl, the console) and run lab.sh down again. If hangar cannot"
            warn "  say (a module down: lab.sh status) and QGroundControl shows it disarmed, or it has to"
            warn "  come down as it is: lab.sh down --force (the mavlink module sends neutral and a disarm"
            warn "  as it stops)."
            exit 1
        fi
    fi
    step "the laptop's hellion and eeye stopped" laptop_video_down
    tunnels_down

    # No session file here: the Pi's copy of it, if up left one. With neither, what up
    # changed is not known, and down says so at the end instead of "as found".
    if [ ! -f "$SESSION" ]; then
        if sess_pull; then
            say "no session file on this laptop: taken from the Pi's copy (open since $(sess_get started))"
        else
            lost=1
        fi
    fi

    local backup; backup=$(sess_get params_backup)
    if [ -n "$backup" ]; then
        # The restore goes the way the save went, through hangar: up again for it if it
        # stopped. It ends with a comparison against the backup: its status is the
        # step's. (A backup of an older session, MAVROS's YAML, restores the same way.)
        if ! hangar_running; then
            step "hangar up again for the restore" hangar_up_again
        fi
        step "ArduSub's parameters as the lab left them (~/$backup)" \
            rsh FILE="$backup" '"$ROV" params restore "$HOME/$FILE" > "$HOME/hangar-rov/restore.log" 2>&1; rc=$?
                grep -vE "^ {4}|^Set parameter" "$HOME/hangar-rov/restore.log"; exit $rc'
    elif [ "$lost" = 0 ]; then
        say "no parameter backup in the session: ArduSub's parameters were not changed by lab.sh"
    fi
    # The topside on this laptop, after the disarm and the restore (neither needs it) and
    # before the vehicle's reset, which, rehearsing on one machine, would stop it first.
    if [ "$(sess_get topside)" = 1 ] || topside_laid_out; then
        step "the topside role on this laptop stopped, its root removed" eval 'LAB_FORCE=$force topside reset > /dev/null'
    fi
    step "hangar and the relay stopped, the roots removed" rsh ROV_FORCE="$force" '"$ROV" reset > /dev/null'
    # The lab's own services, unless the Pi is frozen: the freeze holds them stopped
    # for the measurements, and thaw starts the ones that ran when it began.
    local services held="" rec; services=$(sess_get services_stopped)
    rec=$(frozen_record)
    if [ -n "$services" ] && [ -n "$rec" ]; then
        local s keep=""
        for s in $services; do
            if grep -qx "stopped $s" <<< "$rec"; then held+="$s "; else keep+="$s "; fi
        done
        services=${keep% }
        [ -z "$held" ] || say "ok    the lab's ${held% } left stopped: the Pi is frozen, and lab.sh thaw starts them"
    fi
    if [ -n "$services" ]; then
        # shellcheck disable=SC2086
        step "the lab's $services started again" remote_sudo systemctl start $services
    fi
    if [ "$lost" = 1 ]; then
        [ -z "$rec" ] || frozen_notice
        down_without_session "$was_up" "${#failed[@]}"
        return
    fi
    if [ "${#failed[@]}" -eq 0 ]; then
        step "the Pi's copy of the session removed" rsh 'rm -f "$HOME/hangar-rov/lab-session"'
    fi
    is_local || ssh "${SSH_OPTS[@]}" -O exit "$TARGET" 2>/dev/null

    if [ "${#failed[@]}" -eq 0 ]; then
        rm -f "$SESSION" "$LAB_STATE_DIR/eeye.json"
        if [ -n "$rec" ]; then
            say "the session is down; NOT everything is as found yet:"
            warn "THE PI IS FROZEN (since $(sed -n 's/^frozen_at //p' <<< "$rec" | head -1)): lab.sh thaw before leaving (a reboot undoes it too)"
        else
            say "everything as found"
        fi
    else
        warn "not everything could be put back: ${failed[*]}"
        [ -z "$rec" ] || frozen_notice
        warn "the session file ($SESSION) is kept: fix what failed and run lab.sh down again"
        # The Pi's rov.sh asks once more as it stops: its refusal (NOT DONE, above) is an
        # arm, or a module gone, since down's own check.
        [[ " ${failed[*]} " != *" stopped"* ]] ||
            warn "if rov.sh refused above (NOT DONE: the vehicle is not disarmed, or cannot say): disarm and down again, or lab.sh down --force"
        exit 1
    fi
}

# The baselines of bench/pairs to the Pi (tools/baselines.sh, tools/baselines-debs.sh):
# unpacked beside hangar's install, never into it, and nothing fetched from there. The
# .deb files are installed by dpkg from the files themselves: no `apt update`, which is
# what would start the package machinery a frozen Pi keeps quiet.
cmd_baselines() {
    local debs= tarball=
    while [ $# -gt 0 ]; do
        case $1 in
            --debs) shift; debs=${1:-}; [ -n "$debs" ] || usage 2 ;;
            -*) usage 2 ;;
            *) tarball=$1 ;;
        esac
        shift
    done
    [ -n "$tarball" ] || tarball=$(ls -t "$here"/../../out/baselines/baselines-*-aarch64.tar.zst 2>/dev/null | head -1)
    [ -n "$tarball" ] && [ -f "$tarball" ] || die "no baselines archive: tools/baselines.sh makes one (or name it)"
    [ -z "$debs" ] || [ -f "$debs/SHA256SUMS" ] || die "$debs has no SHA256SUMS: tools/baselines-debs.sh --out $debs"
    reachable || exit 1
    local name; name=$(basename "$tarball" .tar.zst)
    say "the baselines $name to the Pi (~/hangar-baselines)"
    rsh 'mkdir -p "$HOME/hangar-baselines"' || die "the Pi did not answer"
    copy_to "$tarball" hangar-baselines || die "could not copy $tarball"
    rsh NAME="$name" 'cd "$HOME/hangar-baselines" && tar --zstd -xf "$NAME.tar.zst" && rm -f "$NAME.tar.zst" && ln -sfn "$NAME" current &&
        cd current && bad=0 &&
        { sed -n "/^files\$/,\$p" MANIFEST | tail -n +2 | sha256sum -c --quiet - || bad=1; } &&
        [ $bad = 0 ] && echo "lab: every file as its MANIFEST says"' || die "the archive did not unpack as its MANIFEST says"
    # Each node says what it is, where it will run. One that cannot run is the finding.
    rsh 'cd "$HOME/hangar-baselines/current" || exit 1
        for n in bin/*; do
            if v=$("$n" --version 2>&1); then echo "lab:   ok    $(basename "$n"): $(echo "$v" | sed -n "s/.*\"version\": \"\([^\"]*\)\".*/\1/p")"
            else echo "lab:   FAIL  $(basename "$n") does not run here: $(echo "$v" | head -1)"; fi
        done
        if [ -d ros2/install ]; then
            if [ -r /opt/ros/jazzy/setup.sh ]; then
                if v=$(. /opt/ros/jazzy/setup.sh && . ros2/install/setup.sh && ros2_pair_node --version 2>&1); then
                    echo "lab:   ok    ros2_pair_node (prebuilt) loads against this ROS 2: $(echo "$v" | sed -n "s/.*\"version\": \"\([^\"]*\)\".*/\1/p")"
                else
                    echo "lab:   FAIL  the prebuilt ros2_pair_node does not load against this ROS 2: $(echo "$v" | tail -1)"
                    echo "lab:         build it here, once, before freezing: . /opt/ros/jazzy/setup.sh && sh ~/hangar-baselines/current/ros2-build.sh"
                fi
            else echo "lab:   skip  no /opt/ros/jazzy here: the ROS 2 entries will be skipped"; fi
        fi'
    if [ -n "$debs" ]; then
        (cd "$debs" && sha256sum -c --quiet SHA256SUMS) || die "$debs: a file is not what SHA256SUMS says"
        local n; n=$(wc -l < "$debs/SHA256SUMS")
        say "$n .deb files to the Pi (~/hangar-baselines/debs), then, as root there:"
        say "    dpkg -i ~/hangar-baselines/debs/*.deb"
        [ -f "$debs/README.txt" ] && sed 's/^/lab:     /' "$debs/README.txt" | head -40
        rsh 'rm -rf "$HOME/hangar-baselines/debs" && mkdir -p "$HOME/hangar-baselines/debs"'
        local f
        for f in "$debs"/*.deb "$debs/SHA256SUMS"; do copy_to "$f" hangar-baselines/debs || die "could not copy $f"; done
        rsh 'cd "$HOME/hangar-baselines/debs" && sha256sum -c --quiet SHA256SUMS' || die "a .deb did not arrive whole"
        say "sudo on the Pi, once, for the dpkg -i above"
        if is_local; then say "rehearsal, skipped: sudo dpkg -i (nothing is installed on this laptop)"
        else
            # shellcheck disable=SC2016
            remote_sudo sh -c 'dpkg -i /home/'"$LAB_USER"'/hangar-baselines/debs/*.deb' ||
                warn "dpkg -i did not end well: read what it said; nothing else was changed"
        fi
    fi
    say "on the Pi: ~/hangar-baselines/current (bench/pairs/README.md, \"On the Pi\")"
}

case "${1:-}" in
    baselines) shift; cmd_baselines "$@" ;;
    deploy) shift; cmd_deploy "$@" ;;
    up) shift; cmd_up "$@" ;;
    status) cmd_status ;;
    preflight) shift; cmd_preflight "$@" ;;
    bench) shift; cmd_bench "$@" ;;
    camera) shift; cmd_camera "$@" ;;
    down) shift; cmd_down "$@" ;;
    freeze) shift; cmd_freeze "$@" ;;
    thaw) shift; cmd_thaw "$@" ;;
    -h|--help|help|'') usage 0 ;;
    *) warn "unknown command $1"; usage 2 ;;
esac
