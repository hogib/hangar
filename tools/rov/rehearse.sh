#!/usr/bin/env bash
# tools/rov/rehearse.sh: lab.sh's sessions run on this laptop, without the vehicle, and
# checked (tools/rov/QUICKSTART.md, "Rehearsing without the vehicle"). LAB_HOST=local:
# both roles here, from an x86-64 release (tools/release.sh --host) built at this
# checkout's commit, in a home of its own that is removed at the end; nothing of the
# user's ~/hangar-rov, ~/hangar-labtest or ~/.cache/hangar-lab is read or written.
#
#   rehearse.sh [-k] TARBALL [plain] [bridge] [serial] [both]     (default: all four)
#
#   plain    up, status, down: both roles on "the Pi", the shim, no Pixhawk
#   bridge   up --bridge: the vehicle role and the topside role joined by the bridge on
#            loopback (UDP 5614 and 5615); status and bench on both ends; a release with
#            another msg/ tree refused before anything starts; down from the Pi's copy
#            of the session
#   serial   up --serial: the mavlink module on a pseudo-terminal with the tests' fake
#            ArduSub at its far end; refused while something else holds the line;
#            ArduSub's parameters saved, set, and restored by a down that first has to
#            start hangar again
#   both     up --bridge --serial: armed from the topside across the link, and while
#            armed nothing stops hangar (rov.sh down and reset on the Pi, rov.sh topside
#            down on the laptop, lab.sh up: each refused, each manager still running);
#            with the topside down, arm refused for the link; armed again, and down
#            disarms before it stops anything
#   -k       keep the home and its logs (their place is printed)
#
# Every run ends with lab.sh down, and must end with "lab: everything as found", no
# manager left and both roots gone. It uses rov.sh's roots (/dev/shm/hangar-vehicle and
# -topside) and the profiles' ports, and refuses to start while either is in use.
# The fake ArduSub is the test suite's: make check builds it.
#
# What this cannot show: SSH, sudo and the clock on the Pi, the key's copy over scp, the
# lab's services, MAVROS, the real Pixhawk's serial line and its parameters, the tether.
set -uo pipefail

here=$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")
repo=$(realpath -- "$here/../..")
keep=0
[ "${1:-}" = -k ] && { keep=1; shift; }
case "${1:-}" in ''|-h|--help) sed -n '2,29p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;; esac
tarball=$(realpath -- "$1"); shift
[ -f "$tarball" ] || { echo "rehearse: no file $tarball" >&2; exit 2; }
[ $# -gt 0 ] || set -- plain bridge serial both

RV=/dev/shm/hangar-vehicle
RT=/dev/shm/hangar-topside
[ ! -e "$RV" ] && [ ! -e "$RT" ] || { echo "rehearse: $RV or $RT exists: a hangar of rov.sh's runs here (rov.sh reset, lab.sh down)" >&2; exit 2; }
busy=$(ss -lntuH 2>/dev/null | awk '{ print $5 }' | grep -E ':(8080|8081|5614|5615|14560|14650)$' | sort -u | tr '\n' ' ')
[ -z "$busy" ] || { echo "rehearse: in use on this laptop: $busy" >&2; exit 2; }

H=$(mktemp -d "${TMPDIR:-/tmp}/hangar-rehearse.XXXXXX")
export HOME=$H LAB_HOST=local RESULTS_DIR=$H/results LAB_STATE_DIR=$H/.cache/hangar-lab ROV_ROOT=hangar-labtest
unset RELAY FCU LAB_SERIAL_SERVICES BRIDGE BRIDGE_PORT MAVLINK HANGAR_BIN HANGAR_ROV_STATE HANGAR_ROOT
LAB=$here/lab.sh
VH() { HANGAR_ROOT=$RV "$H/hangar-labtest/bin/hangar" "$@"; }      # the vehicle's hangar
TH() { HANGAR_ROOT=$RT "$repo/builddir/hangar" "$@"; }             # the topside's, with --bridge
FAKE=$(ls "$repo"/builddir/tests-*/mavlink-modules/fake_ardusub 2>/dev/null | head -1)
CTL=24671                                                           # the fake's control port
PTY=$H/pixhawk
fake_pid="" hold_pid="" fails=0 run=""

fake() { # a control line to the fake ArduSub, its answer
    python3 -I -c 'import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
s.sendto(sys.argv[2].encode(), ("127.0.0.1", int(sys.argv[1])))
try: print(s.recv(600).decode().strip())
except OSError: print("no answer")' "$CTL" "$1"
}

cleanup() {
    # --force: whatever state a failed run left the fake vehicle in, nothing stays up.
    [ -f "$LAB_STATE_DIR/session" ] && bash "$LAB" down --force > "$H/log/cleanup-down.txt" 2>&1
    [ -n "$hold_pid" ] && kill "$hold_pid" 2>/dev/null
    [ -n "$fake_pid" ] && kill "$fake_pid" 2>/dev/null
    BRIDGE=1 bash "$here/rov.sh" topside reset --force > /dev/null 2>&1
    [ -x "$H/hangar-labtest/share/hangar/rov.sh" ] && "$H/hangar-labtest/share/hangar/rov.sh" reset --force > /dev/null 2>&1
    if [ "$keep" = 1 ] || [ "$fails" -gt 0 ]; then echo "rehearse: the home and the logs are kept: $H"; else rm -rf "$H"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM
mkdir -p "$H/log"

ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fails=$((fails + 1)); }
# lab NAME ARGS...: lab.sh ARGS, its output in log/NAME.txt, its status in $rc
lab() { local n=$1; shift; bash "$LAB" "$@" > "$H/log/$run-$n.txt" 2>&1 < /dev/null; rc=$?; }
has() { # NAME what regex: the log has a line matching
    if grep -qE -- "$3" "$H/log/$run-$1.txt"; then ok "$2"
    else bad "$2 (wanted /$3/ in $H/log/$run-$1.txt:)"; sed 's/^/        | /' "$H/log/$run-$1.txt" | tail -25; fi
}
hasnt() { if grep -qE -- "$3" "$H/log/$run-$1.txt"; then bad "$2 (found /$3/ in $H/log/$run-$1.txt)"; else ok "$2"; fi; }
is() { # what command...: the command succeeds
    local what=$1; shift
    if "$@" > /dev/null 2>&1; then ok "$what"; else bad "$what"; fi
}
as_found() { # after a down; `frozen`: of a frozen Pi, which is not yet as found
    if [ "${1:-}" = frozen ]; then
        has down "down of a frozen Pi: the session down, and said that the Pi is still frozen" 'THE PI IS FROZEN \(since 20'
        hasnt down "down of a frozen Pi does not say: everything as found" '^lab: everything as found$'
    else
        has down "down ends with: everything as found" '^lab: everything as found$'
    fi
    is "no manager left, both roots gone" eval '[ ! -e "$RV" ] && [ ! -e "$RT" ] && ! pgrep -u "$(id -u)" -f "^$H/hangar-labtest/bin/hangard|^$repo/builddir/hangard" > /dev/null'
    is "the session file gone" eval '[ ! -e "$LAB_STATE_DIR/session" ] && [ ! -e "$H/hangar-rov/lab-session" ]'
}
until_line() { # seconds command...: until the command succeeds
    local t=0 n=$1; shift
    while [ "$t" -lt "$n" ]; do "$@" > /dev/null 2>&1 && return 0; sleep 1; t=$((t + 1)); done
    return 1
}

start_fake() {
    [ -n "$FAKE" ] || return 1
    # It ends by itself after ten minutes, whatever becomes of this script.
    # It knows every parameter of hangar's file (or ardusub-setup would wait for answers
    # for ever), and one is then set to another value: what "the lab" left.
    timeout 600 "$FAKE" --pty "$PTY" --control "127.0.0.1:$CTL" --gcs-sysid 255 \
        --params "$H/hangar-labtest/share/hangar/profiles/rov-vehicle/ardusub.params" > "$H/log/fake_ardusub.log" 2>&1 &
    fake_pid=$!
    until_line 5 test -e "$PTY" || return 1
    fake "set JS_GAIN_DEFAULT 0.5" > /dev/null
}
stop_fake() { [ -n "$fake_pid" ] && kill "$fake_pid" 2>/dev/null; wait "$fake_pid" 2>/dev/null; fake_pid=""; rm -f "$PTY"; }

echo "rehearse: $(basename "$tarball") in $H"
run=deploy
lab deploy deploy "$tarball"
has deploy "deploy installs the release" '^lab: installed '
[ "$rc" = 0 ] || exit 1

for run in "$@"; do
    echo "== $run"
    case $run in
    plain)
        # Frozen first, as a session of measurements begins (README "Freeze and thaw";
        # rehearsing, the Pi that is frozen is a directory).
        lab unfrozen preflight --measure
        has unfrozen "not frozen: the preflight refuses a measurement" 'FAIL +pi +frozen: not frozen'
        lab freeze freeze
        has freeze "freeze: the commands shown before they are run" '# .*systemctl mask --runtime --now apt-daily-upgrade.timer'
        has freeze "freeze: done" '^freeze: frozen\. Until lab.sh thaw'
        lab up up
        has up "up: both roles, no Pixhawk" 'RELAY=0: no Pixhawk'
        has up "up: the preflight first, without a FAIL" '^preflight \(up\): [0-9]+ ok, [0-9]+ warn, 0 FAIL$'
        lab preflight preflight --measure
        has preflight "preflight --measure: a report, the session's ports left out" '^preflight \(measure\): '
        has preflight "preflight --measure: the Pi reads frozen" 'ok +pi +frozen: since 20'
        hasnt preflight "preflight --measure: no port lines" ' pi +(UDP|TCP) [0-9]'
        lab status status
        has status "status: the topside's root on the Pi" 'topside_mavlink'
        has status "status: the Pi is frozen, said" 'THE PI IS FROZEN'
        lab thaw-up thaw
        has thaw-up "thaw with hangar up: refused" 'hangar runs on the Pi: lab.sh down first'
        lab down down
        as_found frozen
        lab thaw thaw
        has thaw "thaw: put back" '^lab: thawed$'
        lab status-thawed status
        hasnt status-thawed "after thaw: status is quiet about it" 'FROZEN' ;;

    bridge)
        # A release whose msg/ tree is not this checkout's: refused, nothing started.
        echo "# one more line" >> "$H/hangar-labtest/share/hangar/msg/wrench.msg"
        lab mismatch up --bridge
        has mismatch "another msg/ tree: said plainly" 'NOT THE SAME BUILD'
        has mismatch "another msg/ tree: which part" 'msg/ trees differ'
        is "another msg/ tree: up fails, nothing started, no session" eval '[ "$rc" != 0 ] && [ ! -e "$RV" ] && [ ! -e "$RT" ] && [ ! -e "$LAB_STATE_DIR/session" ]'
        sed -i '$d' "$H/hangar-labtest/share/hangar/msg/wrench.msg"

        lab up up --bridge
        has up "up --bridge: the builds compared" '^lab: (both ends are the same build|the Pi.s release is commit)'
        has up "up --bridge: the link is up" '^lab: the link is up: up 127\.0\.0\.1:5614'
        is "the vehicle's tether trigger is on" grep -q '^tether_enabled = 1$' "$RV/var/lib/hangar/safety/params"
        is "the key on this laptop is mode 600" eval '[ "$(stat -c %a "$H/hangar-rov/bridge.key")" = 600 ]'
        lab status status
        has status "status: the vehicle's bridge, on the Pi" '^  bridge +ready .* up 127\.0\.0\.1:5615'
        has status "status: the topside's bridge, on this laptop" '^  bridge +ready .* up 127\.0\.0\.1:5614'
        has status "status: the topside's modules under this laptop" '^  topside_mavlink '
        hasnt status "status: no topside root looked for on the Pi" 'no manager under'
        is "a call from the topside crosses (safety.check)" eval 'TH call safety.check | grep -q "^ok "'
        lab bench bench 6
        is "bench: the preflight before and after, beside the numbers" eval 'grep -q "^preflight (measure):" "$RESULTS_DIR"/*/preflight-before.txt "$RESULTS_DIR"/*/preflight-after.txt && grep -q "^Preflight: no FAIL\|NOT CLEAN" "$RESULTS_DIR"/*/README.md'
        has bench "bench: the link from both ends" 'the link, from this laptop: +bridge ready .* up 127'
        is "bench: the link topics saved, config_match in them" eval 'grep -q "config_match" "$RESULTS_DIR"/*/laptop-link-after.txt "$RESULTS_DIR"/*/rov-link-after.txt'
        is "bench: the bridges measured on both ends" eval 'grep -q hangar-bridge "$RESULTS_DIR"/*/rov-procbench.txt && grep -q hangar-bridge "$RESULTS_DIR"/*/laptop-procbench.txt'
        # A second up without --bridge in the same session: the topside here is stopped.
        lab up2 up
        is "up without --bridge after one with: this laptop's topside gone, the shim's in its place" eval '[ ! -e "$RT/etc/hangar/modules/bridge.module" ] && [ -e "$RT/hangard.pid" ]'
        lab up3 up --bridge
        has up3 "up --bridge again: the link is up" '^lab: the link is up'
        # The laptop's session file lost: down goes by the Pi's copy, the topside in it.
        rm -f "$LAB_STATE_DIR/session"
        lab down down
        has down "down without this laptop's session file: the Pi's copy taken" "taken from the Pi's copy"
        has down "down: the topside here stopped" '^lab: ok    the topside role on this laptop stopped'
        as_found ;;

    serial|both)
        if ! start_fake; then bad "no fake ArduSub (make check builds builddir/tests-*/mavlink-modules/fake_ardusub): $run NOT RUN"; continue; fi
        export FCU=$PTY:115200
        # A unit of the lab's named as the line's holder: stopped by up, started by down
        # (rehearsing, sudo is skipped and said; the session's record of it is real).
        [ "$run" = serial ] && export LAB_SERIAL_SERVICES=lab-mavros
        before=$(fake "get JS_GAIN_DEFAULT")
        flags=(--serial); [ "$run" = both ] && flags+=(--bridge)

        if [ "$run" = serial ]; then
            # Something else holds the line: refused before anything is changed.
            sleep 120 < "$PTY" & hold_pid=$!
            lab held up "${flags[@]}"
            has held "the line held by another process: refused" 'is held by something else'
            has held "the line held: the holder named" "pid $hold_pid, sleep"
            is "the line held: up fails, nothing started, no session" eval '[ "$rc" != 0 ] && [ ! -e "$RV" ] && [ ! -e "$LAB_STATE_DIR/session" ]'
            kill "$hold_pid"; wait "$hold_pid" 2>/dev/null; hold_pid=""
        fi

        lab up up "${flags[@]}"
        [ "$run" = serial ] && has up "up --serial: LAB_SERIAL_SERVICES stopped" 'skipped: sudo systemctl stop lab-mavros'
        [ "$run" = serial ] && is "the stopped unit is in the session, here and on the Pi" grep -q '^services_stopped=lab-mavros$' "$LAB_STATE_DIR/session" "$H/hangar-rov/lab-session"
        has up "up ${flags[*]}: the parameters saved" "^lab: saving ArduSub's parameters"
        has up "up ${flags[*]}: READY TO ARM" '^lab: READY TO ARM'
        is "the profile's mavlink files are as in the release" eval '(cd "$H/hangar-visit"/*/share/hangar/profiles/rov-vehicle && cmp -s mavlink.conf "$H/hangar-labtest/share/hangar/profiles/rov-vehicle/mavlink.conf" && cmp -s mavlink.module "$H/hangar-labtest/share/hangar/profiles/rov-vehicle/mavlink.module")'
        is "the module is on the serial line" eval 'VH status | grep -E "^mavlink +ready .* serial open"'
        is "no relay runs" eval '[ ! -e "$H/hangar-rov/relay.pid" ]'
        set_to=$(fake "get JS_GAIN_DEFAULT")
        is "ardusub-setup wrote to the fake (JS_GAIN_DEFAULT $before -> $set_to)" eval '[ "$before" != "$set_to" ]'
        lab status status
        has status "status: the serial line said" 'the mavlink module holds .*pixhawk itself'

        if [ "$run" = both ]; then
            is "armed from the topside, across the link" eval 'TH call safety.arm && until_line 5 eval "VH status | grep -E \"^safety .* armed\""'
            # The guard (README "The guard"), before the real stack, armed: nothing that
            # stops hangar is done, on either end, and every manager is still the same one.
            vpid=$(cat "$RV/hangard.pid"); tpid=$(cat "$RT/hangard.pid")
            "$H/hangar-labtest/share/hangar/rov.sh" armed > "$H/log/$run-armed.txt" 2>&1; grc=$?
            has armed "armed: rov.sh armed on the Pi says so" '^armed: safety: armed; ArduSub: armed'
            is "armed: its status is 4" eval '[ "$grc" = 4 ]'
            "$H/hangar-labtest/share/hangar/rov.sh" down > "$H/log/$run-guard-down.txt" 2>&1; grc=$?
            has guard-down "armed: rov.sh down on the Pi refused" 'NOT DONE \(stopping hangar\): the vehicle is not disarmed'
            is "armed: its status is 4" eval '[ "$grc" = 4 ]'
            "$H/hangar-labtest/share/hangar/rov.sh" reset > "$H/log/$run-guard-reset.txt" 2>&1
            has guard-reset "armed: rov.sh reset on the Pi refused" 'NOT DONE \(stopping hangar and removing its roots\)'
            BRIDGE=1 bash "$here/rov.sh" topside down > "$H/log/$run-guard-topside.txt" 2>&1; grc=$?
            has guard-topside "armed: rov.sh topside down on the laptop refused (the bridge's copies read)" 'NOT DONE \(stopping the topside role\): the vehicle is not disarmed \(safety: armed; ArduSub: armed\)'
            is "armed: its status is 4" eval '[ "$grc" = 4 ]'
            lab guard-up up "${flags[@]}"
            has guard-up "armed: lab.sh up refused, nothing changed" 'THE VEHICLE IS NOT DISARMED'
            is "armed: lab.sh up failed" eval '[ "$rc" != 0 ]'
            is "armed: both managers are the ones that ran before, the vehicle still armed" eval '[ "$(cat "$RV/hangard.pid")" = "$vpid" ] && [ "$(cat "$RT/hangard.pid")" = "$tpid" ] && kill -0 "$vpid" "$tpid" && VH status | grep -E "^safety .* armed"'
            is "disarmed from the topside" eval 'TH call safety.disarm && until_line 5 eval "VH status | grep -E \"^safety .*disarmed\""'
            is "disarmed: rov.sh armed on the Pi says so" until_line 5 eval '"$H/hangar-labtest/share/hangar/rov.sh" armed | grep -E "^disarmed: safety: disarmed; ArduSub: disarmed"'
            # The one-second limit's check. A machine that grants no real time (this
            # one, CI's) has nothing to try: said, status 3, and nothing left spinning.
            "$H/hangar-labtest/share/hangar/rov.sh" spin > "$H/log/$run-spin.txt" 2>&1; grc=$?
            has spin "spin: says what it found" 'spin: (real time is not in force here|ended after about .* the limit holds)'
            is "spin: status $grc, its manifest gone, nothing left spinning" eval '{ [ "$grc" = 3 ] || [ "$grc" = 0 ]; } && [ ! -e "$RV/etc/hangar/modules/spin.module" ] && ! pgrep -x spin'
            # The vehicle's bridge calls the link lost after about 3 s of silence; an arm
            # sent before that is still taken (and the tether trigger then ends it).
            BRIDGE=1 bash "$here/rov.sh" topside down > /dev/null 2>&1
            t0=$(date +%s)
            is "the topside down: the vehicle's bridge says so within 10 s" until_line 10 eval '! VH status | grep -qE "^bridge +ready .* up 127"'
            echo "        (after about $(($(date +%s) - t0)) s)"
            VH call safety.arm > "$H/log/$run-arm.txt" 2>&1; arc=$?
            echo "        the vehicle's answer to arm with the topside down: $(tr '\n' ' ' < "$H/log/$run-arm.txt" | cut -c1-160)"
            is "the topside down: arm refused" eval '[ "$arc" != 0 ] && ! VH status | grep -qE "^safety .* armed"'
            has arm "the topside down: the refusal names the link" 'bridge\.vehicle\.link|[Tt]ether'
            lab status-nolink status
            has status-nolink "status: the missing topside said" 'no manager under /dev/shm/hangar-topside \(the topside role'
            BRIDGE=1 bash "$here/rov.sh" topside up > /dev/null 2>&1
            is "the topside up again: the link back" until_line 20 eval 'TH status | grep -E "^bridge +ready .* up 127"'
            # Armed when down is asked for: down disarms, reads the state again, and
            # only then stops anything.
            is "armed again, for down" eval 'until_line 20 TH call safety.arm && until_line 5 eval "VH status | grep -E \"^safety .* armed\""'
        fi

        if [ "$run" = serial ]; then
            # hangar gone from the Pi before down: started again for the restore, on the
            # serial line as the session says.
            "$H/hangar-labtest/share/hangar/rov.sh" reset > /dev/null 2>&1
        fi
        lab down down
        [ "$run" = both ] && has down "down on an armed vehicle: said, and disarmed before anything is stopped" 'found NOT DISARMED \(safety: armed; ArduSub: armed\): disarming it'
        [ "$run" = both ] && has down "down: the vehicle read disarmed" '^lab: ok    disarmed \(safety: disarmed; ArduSub: disarmed'
        [ "$run" = serial ] && has down "down after hangar stopped: up again, on the serial line, for the restore" '^lab: ok    hangar up again for the restore'
        has down "down: the parameters restored and compared" "^lab: ok    ArduSub's parameters as the lab left them"
        [ "$run" = serial ] && has down "down: the stopped unit started again" "^lab: ok    the lab's lab-mavros started again"
        after=$(fake "get JS_GAIN_DEFAULT")
        is "the fake has its own value again (JS_GAIN_DEFAULT $after)" eval '[ "$before" = "$after" ] && [ "$before" != "no answer" ]'
        as_found
        stop_fake
        unset FCU LAB_SERIAL_SERVICES ;;

    *) bad "no rehearsal called $run" ;;
    esac
done

echo
if [ "$fails" = 0 ]; then echo "rehearse: all passed ($*)"; else echo "rehearse: $fails FAILED"; exit 1; fi
