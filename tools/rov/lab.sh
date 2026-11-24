#!/usr/bin/env bash
# tools/rov/lab.sh: a lab session with the lab's BlueROV2, from the laptop, in five
# commands (tools/rov/QUICKSTART.md; issue #4). It drives the vehicle's Pi over SSH and
# keeps what it changed in a session file, so that down puts everything back.
#
#   lab.sh deploy [TARBALL]          copy a release to the Pi, check it, install it,
#                                    set the Pi's clock (default: the newest aarch64
#                                    tarball under out/release/)
#   lab.sh up [--video] [--bridge] [--serial] [--no-battery-check]
#                                    hangar up on the Pi, ArduSub's parameters saved
#                                    and set, the pre-arm check; consoles tunnelled;
#                                    --video: the camera's own H.264, the lab's
#                                    electric-eye stopped, the laptop's video started;
#                                    --bridge: the topside role on this laptop, joined
#                                    to the vehicle by the tether bridge (the tether
#                                    trigger on: arming needs the link);
#                                    --serial: the mavlink module on the Pixhawk's
#                                    serial line itself, no MAVROS relay
#   lab.sh status                    what runs where, armed or not, temperature
#   lab.sh bench [SECONDS]           CPU, memory, frame rates on both, to RESULTS_DIR
#   lab.sh down                      everything put back as found
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

usage() { sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
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
             BRIDGE="$1" BRIDGE_PORT="$V_PORT" BRIDGE_PEER_PORT="$T_PORT")
    if [ "$2" = 1 ]; then ROV_ENV+=(MAVLINK=serial RELAY=0); fi
}

# The topside role on this laptop, for --bridge: the checkout's rov.sh and builddir/.
topside() { # up|down|status|reset
    BRIDGE=1 ROV_IP="$ROV_IP" GCS_IP="$LAPTOP_IP" BRIDGE_PORT="$T_PORT" BRIDGE_PEER_PORT="$V_PORT" \
        bash "$here/rov.sh" topside "$1"
}
topside_hangar() { HANGAR_ROOT=$RT "$repo/builddir/hangar" "$@"; }
# A topside root laid out with the bridge is this laptop's own (the shim's, rehearsing,
# has no bridge in it).
topside_laid_out() { [ -e "$RT/etc/hangar/modules/bridge.module" ]; }
bridge_session() { [ "$(sess_get bridge)" = 1 ] || topside_laid_out; }
topside_line() { topside_hangar status 2>/dev/null | awk -v m="$1" '$1 == m { $1 = $1; print }'; }

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

laptop_pipelines() { # hellion check's pipeline lines, the frames each got in its window
    timeout 6 "$HELLION" check 2>/dev/null | grep -E '\] pipeline ' | sed 's/^/  /'
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
    local video=0 battery=1 bridge=0 serial=0 pixhawk=0 out rc
    for a in "$@"; do
        case $a in
            --video) video=1 ;;
            --bridge) bridge=1 ;;
            --serial) serial=1 ;;
            --no-battery-check) battery=0 ;;
            *) usage 2 ;;
        esac
    done
    reachable || exit 1
    rsh '[ -x "$ROV" ] && [ -x "$B/hangard" ]' || die "no hangar at $ROV_ROOT on the Pi: lab.sh deploy first"
    if [ "$video" = 1 ]; then
        [ -x "$HELLION" ] && [ -x "$EEYE" ] || die "no $HELLION or $EEYE: run make -j8 all in $repo"
        ss -lnt "( sport = :$HELLION_CONTROL_PORT )" 2>/dev/null | grep -q LISTEN &&
            ! alive "$LAB_STATE_DIR/hellion.pid" hellion &&
            die "port $HELLION_CONTROL_PORT is taken on this laptop: set HELLION_CONTROL_PORT"
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
        laptop_pipelines
    fi

    local check
    check=$(rsh 'timeout 30 "$B/hangar" check 2>&1 | grep -E "^(ok|detail) "')
    echo
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
}

# ---- bench ---------------------------------------------------------------------------

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
    [ "$bridge" = 1 ] && link_counters after
    {
        echo "# Bench, $(date -Is)"
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
    local failed=() was_up=0 lost=0
    step() { # what command...: never stops at a failure, says which ones failed
        local what=$1; shift
        if "$@"; then say "ok    $what"; else warn "FAILED $what"; failed+=("$what"); fi
    }
    step "the laptop's hellion and eeye stopped" laptop_video_down
    if ! reachable; then
        warn "the Pi is out of reach: its side is left as it is; the session file is kept for the next down"
        topside_laid_out && warn "the topside role on this laptop is left running too: it is the vehicle's link when the tether is back"
        exit 1
    fi
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

    if hangar_running; then
        was_up=1
        # The call's own status, not the sleep's (the three seconds are ArduSub's, to
        # report itself disarmed before the restore asks).
        step "disarmed" rsh '"$B/hangar" call safety.disarm > /dev/null; rc=$?; sleep 3; exit $rc'
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
        step "the topside role on this laptop stopped, its root removed" eval 'topside reset > /dev/null'
    fi
    step "hangar and the relay stopped, the roots removed" rsh '"$ROV" reset > /dev/null'
    local services; services=$(sess_get services_stopped)
    if [ -n "$services" ]; then
        # shellcheck disable=SC2086
        step "the lab's $services started again" remote_sudo systemctl start $services
    fi
    if [ "$lost" = 1 ]; then
        down_without_session "$was_up" "${#failed[@]}"
        return
    fi
    if [ "${#failed[@]}" -eq 0 ]; then
        step "the Pi's copy of the session removed" rsh 'rm -f "$HOME/hangar-rov/lab-session"'
    fi
    is_local || ssh "${SSH_OPTS[@]}" -O exit "$TARGET" 2>/dev/null

    if [ "${#failed[@]}" -eq 0 ]; then
        rm -f "$SESSION" "$LAB_STATE_DIR/eeye.json"
        say "everything as found"
    else
        warn "not everything could be put back: ${failed[*]}"
        warn "the session file ($SESSION) is kept: fix what failed and run lab.sh down again"
        exit 1
    fi
}

case "${1:-}" in
    deploy) shift; cmd_deploy "$@" ;;
    up) shift; cmd_up "$@" ;;
    status) cmd_status ;;
    bench) shift; cmd_bench "$@" ;;
    down) cmd_down ;;
    -h|--help|help|'') usage 0 ;;
    *) warn "unknown command $1"; usage 2 ;;
esac
