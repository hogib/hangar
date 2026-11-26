#!/usr/bin/env bash
# tools/rov/tests/test_guard.sh: rov.sh's guard (tools/rov/README.md, "The guard"),
# every case of it, without a vehicle and without hangar: `make check-rov`.
#
# rov.sh runs against two roots of this test's own (HANGAR_ROV_VEHICLE_ROOT and
# HANGAR_ROV_TOPSIDE_ROOT, so that a session's /dev/shm/hangar-vehicle is never met), a
# `sleep` in the manager's place under each (it ends by itself after five minutes,
# whatever becomes of this script) and, for $HANGAR_BIN/hangar, a stub that answers
# `hangar echo <topic> --json --count 2 ...` from the file <root>/stub/<topic>:
#
#   a JSON line     printed twice: the topic is being published, and says this
#   no file         "cannot subscribe", status 1: no such topic
#   the word once   one message and then silence: the last word of a module that died
#
# What this does not show is that the real `hangar echo` prints what the stub prints:
# tools/rov/rehearse.sh's `both` run puts the same guard before a real stack, armed.
set -uo pipefail

here=$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")
ROV=$here/../rov.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/hangar-guard.XXXXXX")
pids=()
cleanup() {
    local p
    for p in ${pids[@]+"${pids[@]}"}; do kill "$p" 2>/dev/null; done
    rm -rf "$T"
}
trap cleanup EXIT

mkdir -p "$T/bin" "$T/state"
cat > "$T/bin/hangar" <<'STUB'
#!/usr/bin/env bash
[ "${1:-}" = echo ] || { echo "stub: hangar $*" >&2; exit 2; }
f=$HANGAR_ROOT/stub/$2
[ -f "$f" ] || { echo "hangar: cannot subscribe to $2: No such file or directory" >&2; exit 1; }
if [ "$(head -c 4 "$f")" = once ]; then
    echo '{"seq":7,"stamp_ns":1,"publish_ns":1,"stale":false,"state":0,"outputs_enabled":0,"armed":0,"last_rx_ns":5}'
    exec sleep 30
fi
cat "$f"; cat "$f"
STUB
chmod +x "$T/bin/hangar"

export HANGAR_BIN=$T/bin HANGAR_ROV_STATE=$T/state HANGAR_SESSIONS=$T/sessions
export HANGAR_ROV_VEHICLE_ROOT=$T/vehicle HANGAR_ROV_TOPSIDE_ROOT=$T/topside
unset ROV_FORCE ROV_GUARD_WAIT BRIDGE RELAY MAVLINK
RV=$HANGAR_ROV_VEHICLE_ROOT RT=$HANGAR_ROV_TOPSIDE_ROOT

fails=0 n=0
ok() { n=$((n + 1)); echo "  ok    $*"; }
bad() { n=$((n + 1)); fails=$((fails + 1)); echo "  FAIL  $*"; }

# A manager under a root: a sleep, its pid in hangard.pid. With `group`, a process
# group of its own (the relay's is signalled as a group).
manager() { # pid-file [group]
    mkdir -p "$(dirname "$1")"
    if [ "${2:-}" = group ]; then setsid sleep 300 & else sleep 300 & fi
    pids+=($!)
    disown $!        # stopped by rov.sh: the shell's word on that is not the test's
    echo $! > "$1"
}
alive() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }
# The state a root's two topics say: safety STATE OUTPUTS, ardusub ARMED LAST_RX.
says() { # root state outputs armed last_rx
    mkdir -p "$1/stub"
    echo "{\"seq\":9,\"stamp_ns\":1,\"publish_ns\":1,\"stale\":false,\"state\":$2,\"outputs_enabled\":$3}" > "$1/stub/safety.state"
    echo "{\"seq\":9,\"stamp_ns\":1,\"publish_ns\":1,\"stale\":false,\"armed\":$4,\"last_rx_ns\":$5}" > "$1/stub/ardusub.state"
}
# run WHAT STATUS REGEX rov.sh-words...: the status and a line of the output (both streams)
run() {
    local what=$1 want=$2 re=$3 out rc; shift 3
    out=$(bash "$ROV" "$@" 2>&1); rc=$?
    if [ "$rc" = "$want" ] && grep -qE -- "$re" <<< "$out"; then ok "$what"
    else bad "$what (status $rc, wanted $want; wanted /$re/ in:)"; sed 's/^/        | /' <<< "$out"; fi
}
is() { local what=$1; shift; if "$@" > /dev/null 2>&1; then ok "$what"; else bad "$what"; fi; }
isnt() { local what=$1; shift; if "$@" > /dev/null 2>&1; then bad "$what"; else ok "$what"; fi; }
fresh() { rm -rf "$RV" "$RT"; }

echo "== the vehicle's root"
fresh
run "no manager: down proceeds" 0 '^rov: down$' down
run "no manager: armed says so" 0 '^no manager$' armed

fresh; manager "$RV/hangard.pid"; says "$RV" 1 1 1 5000
run "armed: the word" 4 '^armed: safety: armed; ArduSub: armed$' armed
run "armed: down refused, both readings named" 4 'NOT DONE \(stopping hangar\): the vehicle is not disarmed \(safety: armed; ArduSub: armed\)' down
run "armed: the refusal says how to disarm" 4 'call safety\.disarm' down
is "armed: the manager still runs" alive "$RV/hangard.pid"
run "armed: reset refused" 4 'NOT DONE \(stopping hangar and removing its roots\)' reset
is "armed: reset removed nothing" test -f "$RV/stub/safety.state"
is "armed: the manager still runs after the refused reset" alive "$RV/hangard.pid"

says "$RV" 0 0 1 5000
run "safety disarmed, ArduSub armed (a stray arm): refused" 4 'safety: disarmed; ArduSub: armed' down
says "$RV" 0 0 2 5000
run "ArduSub still disarming: refused" 4 'ArduSub: disarming' down
says "$RV" 3 0 0 5000
run "arming: refused" 4 'safety: arming' down
says "$RV" 2 1 1 5000
run "a failsafe that drives (surface, hold): refused" 4 'failsafe, outputs ON' down
says "$RV" 2 0 1 5000
run "a neutral failsafe with ArduSub still armed: refused" 4 'failsafe, outputs off; ArduSub: armed' down
is "after six refusals the manager still runs" alive "$RV/hangard.pid"

says "$RV" 1 1 1 5000
run "armed: down --force proceeds and says what it overrode" 0 '^rov: --force: stopping hangar all the same \(safety: armed; ArduSub: armed\)' down --force
isnt "forced: the manager is stopped" alive "$RV/hangard.pid"
fresh; manager "$RV/hangard.pid"; says "$RV" 1 1 1 5000
ROV_FORCE=1 run "armed: ROV_FORCE=1 is --force" 0 '^rov: --force: ' reset
isnt "forced reset: the root is gone" test -e "$RV"

fresh; manager "$RV/hangard.pid"; says "$RV" 0 0 0 5000
run "disarmed: the word" 0 '^disarmed: safety: disarmed; ArduSub: disarmed$' armed
run "disarmed: down proceeds" 0 '^rov: down$' down
isnt "disarmed: the manager is stopped" alive "$RV/hangard.pid"

fresh; manager "$RV/hangard.pid"; says "$RV" 2 0 0 5000
run "a neutral failsafe, ArduSub disarmed: down proceeds" 0 '^rov: down$' down
fresh; manager "$RV/hangard.pid"; says "$RV" 0 0 0 0
run "ArduSub never heard: down proceeds, and it is said" 0 'has not heard ArduSub' down

echo "== a state that cannot be read is not \"disarmed\""
fresh; manager "$RV/hangard.pid"; mkdir -p "$RV/stub"
run "no topic at all: the word" 5 '^unknown: safety\.state is not being published' armed
run "no topic at all: down refused" 4 'cannot be read: safety\.state is not being published' down
says "$RV" 0 0 0 5000; rm "$RV/stub/ardusub.state"
run "safety says disarmed, no ardusub.state: refused" 4 'ardusub\.state is not being published' down
says "$RV" 0 0 0 5000; echo once > "$RV/stub/safety.state"
ROV_GUARD_WAIT=1 run "safety's last message says disarmed, and no other comes: refused" 4 'safety\.state is not being published' down
says "$RV" 0 0 0 5000; echo '{"seq":1}' > "$RV/stub/ardusub.state"
run "a message without the fields: refused" 4 'could not be read' down
is "after four refusals the manager still runs" alive "$RV/hangard.pid"
run "not readable: --force proceeds" 0 '^rov: --force: stopping hangar all the same \(.*could not be read' down --force
isnt "forced: the manager is stopped" alive "$RV/hangard.pid"

echo "== the relay under a manager"
fresh; manager "$RV/hangard.pid"; says "$RV" 1 1 1 5000; manager "$T/state/relay.pid" group
run "armed: relay down refused" 4 'NOT DONE \(stopping the relay\)' relay down
is "armed: the relay still runs" alive "$T/state/relay.pid"
says "$RV" 0 0 0 5000
run "disarmed: relay down proceeds" 0 '^rov: relay stopped$' relay down
isnt "disarmed: the relay is stopped" alive "$T/state/relay.pid"
says "$RV" 1 1 1 5000
run "armed, no relay running: relay down has nothing to stop or to ask" 0 '^$' relay down
run "the vehicle's manager stopped (forced)" 0 '^rov: down$' down --force

echo "== the topside's root on the laptop (BRIDGE=1)"
export BRIDGE=1
fresh; manager "$RT/hangard.pid"; mkdir -p "$RT/etc/hangar/modules"; : > "$RT/etc/hangar/modules/bridge.module"
says "$RT" 1 1 1 5000
run "armed across the link: topside down refused, the tether failsafe named" 4 'pilot.s$' topside down
run "armed across the link: topside reset refused" 4 'NOT DONE \(stopping the topside role and removing its root\)' topside reset
run "armed across the link: down on the laptop refused too" 4 'NOT DONE \(stopping hangar\)' down
run "armed across the link: armed reads the bridge's copies" 4 '^armed: ' armed
is "armed across the link: the topside still runs, its root is there" eval 'alive "$RT/hangard.pid" && [ -e "$RT/etc/hangar/modules/bridge.module" ]'
run "armed across the link: topside down --force proceeds" 0 '^rov: topside down$' topside down --force
isnt "forced: the topside is stopped" alive "$RT/hangard.pid"

manager "$RT/hangard.pid"; says "$RT" 0 0 0 5000
run "disarmed across the link: topside down proceeds" 0 '^rov: topside down$' topside down
manager "$RT/hangard.pid"; rm -rf "$RT/stub"; mkdir -p "$RT/stub"
run "the link down (no copies arrive): topside reset proceeds, and it is said" 0 'the link is down already' topside reset
isnt "the link down: the topside's root is gone" test -e "$RT"
unset BRIDGE

echo
if [ "$fails" = 0 ]; then echo "guard: all $n passed"; else echo "guard: $fails of $n FAILED"; exit 1; fi
