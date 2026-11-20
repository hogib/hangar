#!/usr/bin/env bash
# tools/rov/lab.sh: a lab session with the lab's BlueROV2, from the laptop, in five
# commands (tools/rov/QUICKSTART.md; issue #4). It drives the vehicle's Pi over SSH and
# keeps what it changed in a session file, so that down puts everything back.
#
#   lab.sh deploy [TARBALL]          copy a release to the Pi, check it, install it,
#                                    set the Pi's clock (default: the newest aarch64
#                                    tarball under out/release/)
#   lab.sh up [--video] [--no-battery-check]
#                                    hangar up on the Pi, ArduSub's parameters saved
#                                    and set, the pre-arm check; consoles tunnelled;
#                                    --video: the camera's own H.264, the lab's
#                                    electric-eye stopped, the laptop's video started
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
HELLION=$repo/builddir/hellion
EEYE=$repo/builddir/eeye
mkdir -p "$LAB_STATE_DIR"
chmod 700 "$LAB_STATE_DIR"
SSH_OPTS=(-o ControlMaster=auto -o "ControlPath=$LAB_STATE_DIR/ssh-%C" -o ControlPersist=30m
          -o ConnectTimeout=6 -o ServerAliveInterval=5 -o ServerAliveCountMax=3)

usage() { sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
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
    remote_sudo date -u -s "@$(date -u +%s)" > /dev/null
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
tunnels_up() {
    is_local && return 0
    ssh "${SSH_OPTS[@]}" -O forward -L "$CONSOLE_VEHICLE_PORT:127.0.0.1:8080" \
        -L "$CONSOLE_TOPSIDE_PORT:127.0.0.1:8081" "$TARGET" 2>/dev/null &&
        sess_set tunnels 1
}
tunnels_down() {
    is_local && return 0
    ssh "${SSH_OPTS[@]}" -O cancel -L "$CONSOLE_VEHICLE_PORT:127.0.0.1:8080" \
        -L "$CONSOLE_TOPSIDE_PORT:127.0.0.1:8081" "$TARGET" 2>/dev/null || true
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
    local video=0 battery=1
    for a in "$@"; do
        case $a in
            --video) video=1 ;;
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
    [ -f "$SESSION" ] && say "a session is open since $(sess_get started): continuing it"
    [ -f "$SESSION" ] || sess_set started "$(date -Is)"
    set_clock || up_failed

    # The lab's electric-eye holds the camera and would send to the laptop's 5610 too.
    if [ "$video" = 1 ]; then
        say "stopping the lab's $LAB_SERVICES on the Pi (started again by down)"
        remote_sudo systemctl stop $LAB_SERVICES || up_failed
        sess_set services_stopped "$LAB_SERVICES"
    fi

    # hangar, laid out afresh so that the encoder is in it or not as asked.
    local source=$H264_NODE
    [[ $source == *:* ]] || source=uvc:$source
    if [ "$video" = 1 ]; then say "hangar up on the Pi, the encoder sending $source"; else say "hangar up on the Pi"; fi
    rsh ENCODER="$video" ENCODER_SOURCE="$source" ROV_IP="$ROV_IP" GCS_IP="$LAPTOP_IP" RELAY="$RELAY" \
        'mkdir -p "$HOME/hangar-rov"; "$ROV" reset > /dev/null && "$ROV" up > "$HOME/hangar-rov/up.log" 2>&1; rc=$?
         grep -E "^rov: (relay|hangar is up|RELAY)" "$HOME/hangar-rov/up.log"; exit $rc' || up_failed
    sess_set hangar_up 1
    wait_for safety 'ready' 30 || up_failed

    if [ "$RELAY" = 0 ]; then
        say "RELAY=0: no Pixhawk; ArduSub's parameters are not touched"
    else
        wait_for mavlink 'ready .*ardusub (disarmed|armed)' 60 || up_failed
        # The backup of the parameters as the lab left them: once per session, so a
        # second up never saves hangar's own values over it.
        local backup; backup=$(sess_get params_backup)
        if [ -z "$backup" ]; then
            backup=hangar-rov/lab-$(date +%Y%m%d-%H%M%S)-before.yaml
            say "saving ArduSub's parameters to the Pi's ~/$backup"
            rsh FILE="$backup" '"$ROV" params save "$HOME/$FILE"' ||
                { warn "the parameters could not be saved: nothing is written to the Pixhawk without a backup"; up_failed; }
            sess_set params_backup "$backup"
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

    tunnels_up || warn "the consoles' tunnels did not come up (ports $CONSOLE_VEHICLE_PORT, $CONSOLE_TOPSIDE_PORT taken?)"
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
    else
        echo "  consoles   http://127.0.0.1:$CONSOLE_VEHICLE_PORT (vehicle), http://127.0.0.1:$CONSOLE_TOPSIDE_PORT (topside)"
    fi
    [ "$video" = 1 ] && echo "  video      QGroundControl on UDP 5600; the effects on http://127.0.0.1:$HELLION_CONTROL_PORT"
    echo "  QGC        connects by itself (UDP 14550): arm there, drive with the sticks"
    echo "  then       lab.sh status, lab.sh bench, and lab.sh down when done"
}

# ---- status --------------------------------------------------------------------------

cmd_status() {
    reachable || exit 1
    echo "== the Pi ($TARGET)"
    remote SERVICES="$LAB_SERVICES" <<'EOF'
case $ROOT in /*) R=$ROOT ;; *) R=$HOME/$ROOT ;; esac
B=$R/bin
for root in /dev/shm/hangar-vehicle /dev/shm/hangar-topside; do
    if [ -f "$root/hangard.pid" ] && kill -0 "$(cat "$root/hangard.pid")" 2>/dev/null; then
        HANGAR_ROOT=$root "$B/hangar" status 2>/dev/null | grep -vE '^MODULE' | cut -c1-120 | sed "s|^|  |"
    else
        echo "  no manager under $root"
    fi
done
[ -f "$HOME/hangar-rov/relay.pid" ] && kill -0 "$(cat "$HOME/hangar-rov/relay.pid")" 2>/dev/null &&
    echo "  MAVROS relay: running" || echo "  MAVROS relay: not running"
for s in $SERVICES; do printf '  %s: %s\n' "$s" "$(systemctl is-active "$s" 2>/dev/null)"; done
if command -v vcgencmd > /dev/null; then
    echo "  $(vcgencmd measure_temp) $(vcgencmd get_throttled) $(vcgencmd measure_clock arm | sed 's/frequency([0-9]*)=/arm Hz=/')"
fi
echo "  clock $(date -u '+%F %T') UTC, up $(uptime -p | sed 's/^up //')"
EOF
    echo "== this laptop"
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
    {
        echo "# Bench, $(date -Is)"
        echo
        echo "$secs s on both machines, by tools/rov/lab.sh bench (hangar $(git -C "$repo" describe --always 2>/dev/null))."
        echo "The state before and after: state-before.txt, state-after.txt (armed or not, temperature,"
        echo "throttling: a throttled or hot Pi reads high). Frame rates: hellion's \"frames in ~1.2 s\""
        echo "reads ~32 for 30 fps; the encoder's own counter is exact."
        echo
        for f in rov-procbench rov-fps laptop-procbench laptop-fps; do
            echo "## $f"; echo; echo '```'; cat "$dir/$f.txt" 2>/dev/null; echo '```'; echo
        done
    } > "$dir/README.md"
    sed -n '/^## rov-fps/,$p' "$dir/README.md" | grep -E '^  |^## ' ; grep -E 'whole machine|  total' "$dir/rov-procbench.txt" "$dir/laptop-procbench.txt" 2>/dev/null | sed "s|$dir/||"
    say "saved in $dir"
}

# ---- down ----------------------------------------------------------------------------

cmd_down() {
    local failed=()
    step() { # what command...: never stops at a failure, says which ones failed
        local what=$1; shift
        if "$@"; then say "ok    $what"; else warn "FAILED $what"; failed+=("$what"); fi
    }
    step "the laptop's hellion and eeye stopped" laptop_video_down
    if ! reachable; then
        warn "the Pi is out of reach: its side is left as it is; the session file is kept for the next down"
        exit 1
    fi
    tunnels_down

    if hangar_running; then
        step "disarmed" rsh '"$B/hangar" call safety.disarm > /dev/null; sleep 3'
    fi
    local backup; backup=$(sess_get params_backup)
    if [ -n "$backup" ]; then
        # The relay is needed to restore: up again if hangar is already down. The
        # restore ends with a comparison against the backup: its status is the step's.
        step "ArduSub's parameters as the lab left them (~/$backup)" \
            rsh RELAY="$RELAY" FILE="$backup" '"$ROV" relay up > /dev/null
                "$ROV" params restore "$HOME/$FILE" > "$HOME/hangar-rov/restore.log" 2>&1; rc=$?
                grep -vE "^ {4}|^Set parameter" "$HOME/hangar-rov/restore.log"; exit $rc'
    else
        say "no parameter backup in the session: ArduSub's parameters were not changed by lab.sh"
    fi
    step "hangar and the relay stopped, the roots removed" rsh '"$ROV" reset > /dev/null'
    local services; services=$(sess_get services_stopped)
    if [ -n "$services" ]; then
        step "the lab's $services started again" remote_sudo systemctl start $services
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
