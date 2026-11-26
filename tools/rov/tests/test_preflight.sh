#!/usr/bin/env bash
# tools/rov/tests/test_preflight.sh: the preflight (tools/rov/README.md, "Preflight"),
# without a vehicle: `make check-rov`.
#
# The judge is given facts written here, one case a check: what is ok, what warns, what
# is a FAIL, in each of its two modes, and the command it names. The probe is run on
# this machine for what a machine can be made to show without root: every fact present,
# a process of the package system found (a copy of `sleep` under its name, ending by
# itself), Ubuntu's idle unattended-upgrade-shutdown not taken for one, a port held and
# its holder named. lab.sh preflight is run once as a rehearsal (LAB_HOST=local).
#
# What this does not show: the Pi's own readings (vcgencmd, its thermal zone), the two
# machines' clocks held against each other over SSH, and file capabilities on hangard.
set -uo pipefail

here=$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")
PF=$here/../preflight.py
T=$(mktemp -d "${TMPDIR:-/tmp}/hangar-preflight.XXXXXX")
pids=()
cleanup() {
    local p
    for p in ${pids[@]+"${pids[@]}"}; do kill "$p" 2>/dev/null; done
    rm -rf "$T"
}
trap cleanup EXIT

fails=0 n=0
ok() { n=$((n + 1)); echo "  ok    $*"; }
bad() { n=$((n + 1)); fails=$((fails + 1)); echo "  FAIL  $*"; }

# facts FILE [python statements changing the dict f]: a clean Pi's facts, changed.
facts() {
    python3 -I - "$1" "${2:-}" <<'EOF'
import json, sys, time
f = {"host": "rov", "user": "bluerov2", "uptime_s": 7200.0, "busy": [{"pct": 4.0, "comm": "hangar-mavlink", "pid": 900}],
     "apt": [], "timers": {"apt-daily.timer": "inactive", "apt-daily-upgrade.timer": "inactive"},
     "temp_c": 55.0, "throttled": 0, "governor": ["performance"], "mem_avail_mb": 6000, "shm_avail_mb": 3900,
     "home_avail_mb": 20000, "ports": [], "services": {"eeye": "inactive", "hellion": "inactive"},
     "release": {"version": "v0.1.6", "commit": "a" * 40},
     "rt": {"rtprio": 0, "memlock": 8388608, "root": False, "hangard": True, "caps": "cap_ipc_lock,cap_sys_nice=ep", "nosuid": False},
     "not_in_force": [], "now": time.time()}
exec(sys.argv[2])
json.dump(f, open(sys.argv[1], "w"))
EOF
}

# judge MODE FILE [more arguments]: the report in $out, the status in $rc.
judge() {
    local mode=$1 file=$2; shift 2
    out=$(python3 -I "$PF" judge --mode "$mode" --pi "$file" "$@" 2>&1); rc=$?
}
# expect NAME STATUS PATTERN...: the status, and every pattern a line of the report.
expect() {
    local name=$1 want=$2 p; shift 2
    [ "$rc" = "$want" ] || { bad "$name: status $rc, expected $want"; echo "$out" | sed 's/^/        /'; return; }
    for p in "$@"; do
        grep -Eq -- "$p" <<< "$out" || { bad "$name: no line matching '$p'"; echo "$out" | sed 's/^/        /'; return; }
    done
    ok "$name"
}
refuse() { # NAME PATTERN: no line of the report matches
    grep -Eq -- "$2" <<< "$out" && { bad "$1: a line matches '$2'"; echo "$out" | sed 's/^/        /'; return; }
    ok "$1"
}

echo "== the judge"
facts "$T/clean.json"
judge up "$T/clean.json";      expect "a clean Pi, before a session" 0 '^preflight \(up\): [0-9]+ ok, 0 warn, 0 FAIL$' 'ok +pi +real time: reachable by file capabilities'
judge measure "$T/clean.json"; expect "a clean Pi, before a measurement" 0 '^preflight \(measure\): [0-9]+ ok, 0 warn, 0 FAIL$'

facts "$T/apt.json" 'f["apt"] = [{"comm": "unattended-upgr", "pid": 4242, "cmdline": "/usr/bin/python3 /usr/bin/unattended-upgrade"}]'
judge up "$T/apt.json";      expect "the package system running: a warning before a session" 0 'warn +pi +package system: running now: unattended-upgr \(pid 4242\)'
judge measure "$T/apt.json"; expect "the package system running: a FAIL before a measurement" 1 'FAIL +pi +package system' 'sudo kill 4242'

facts "$T/timer.json" 'f["timers"]["apt-daily-upgrade.timer"] = "active"; f["uptime_s"] = 600.0'
judge measure "$T/timer.json"; expect "an apt timer active: a warning, with the command" 0 'warn +pi +apt timers: apt-daily-upgrade.timer active, 10 min after boot' 'sudo systemctl stop apt-daily-upgrade.timer'

# The clock: the Pi read 6 days behind this laptop.
facts "$T/clock.json" 'f["host"] = "rov"'
# The laptop's reading of its own clock, as an offset from the instant the Pi's facts
# carry: no time of this test's own is in the comparison.
at() { python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["now"] + float(sys.argv[2]))' "$T/clock.json" "$1"; }
late=$(at 518400)
facts "$T/laptop.json" 'f["host"] = "mousev"; f["rt"]["hangard"] = False; f["release"] = {"version": None, "commit": None}; f["throttled"] = None'
judge up "$T/clock.json" --laptop "$T/laptop.json" --pi-read-at "$late"
expect "a clock days behind: a warning before a session, which sets it" 0 'warn +pi +clock: 518400 s behind' 'lab.sh up sets it'
judge measure "$T/clock.json" --laptop "$T/laptop.json" --pi-read-at "$late"
expect "a clock days behind: a FAIL before a measurement" 1 'FAIL +pi +clock'
judge measure "$T/clock.json" --laptop "$T/laptop.json" --pi-read-at "$(at -30)"
expect "a clock ahead" 1 'FAIL +pi +clock: 30 s ahead of this laptop'
judge measure "$T/clock.json" --laptop "$T/laptop.json" --pi-read-at "$(at 0.3)"
expect "a clock that is right" 0 'ok +pi +clock: 0\.3 s off' 'ok +laptop +package system'

facts "$T/hot.json" 'f["temp_c"] = 82.4; f["throttled"] = 0x80008'
judge measure "$T/hot.json"; expect "at the soft limit: a FAIL before a measurement" 1 'FAIL +pi +temperature: 82.4' 'FAIL +pi +throttling: now: soft temperature limit \(0x80008\)'
judge up "$T/hot.json";      expect "at the soft limit: warnings before a session" 0 'warn +pi +temperature' 'warn +pi +throttling'
facts "$T/warm.json" 'f["temp_c"] = 72.0; f["throttled"] = 0x50000'
judge measure "$T/warm.json"; expect "throttled earlier in this boot: a warning" 0 'warn +pi +temperature: 72.0' 'warn +pi +throttling: not now, but since boot \(0x50000\)'
facts "$T/volt.json" 'f["throttled"] = 0x1'
judge measure "$T/volt.json"; expect "under-voltage now" 1 'FAIL +pi +throttling: now: under-voltage'
facts "$T/laptop-hot.json" 'f["temp_c"] = 88.0; f["throttled"] = None'
judge measure "$T/laptop-hot.json"; expect "a machine without the Pi's throttle word: its temperature is said, not judged" 0 'ok +pi +temperature: 88.0'

facts "$T/gov.json" 'f["governor"] = ["ondemand"]'
judge measure "$T/gov.json"; expect "governor ondemand: a warning before a measurement" 0 'warn +pi +governor: ondemand'
judge up "$T/gov.json";      expect "governor ondemand: nothing to a session" 0 'ok +pi +governor: ondemand'

facts "$T/room.json" 'f["shm_avail_mb"] = 100; f["mem_avail_mb"] = 300; f["home_avail_mb"] = 50'
judge up "$T/room.json"; expect "no room" 1 'FAIL +pi +/dev/shm: 100 MB free' 'warn +pi +memory: 300 MB free' 'FAIL +pi +disk \(home\): 50 MB free'

facts "$T/load.json" 'f["busy"] = [{"pct": 99.0, "comm": "stress", "pid": 77}, {"pct": 97.0, "comm": "hangar-mux", "pid": 78}, {"pct": 60.0, "comm": "hellion", "pid": 79}, {"pct": 28.0, "comm": "mavros_node", "pid": 80, "own": True}]'
judge measure "$T/load.json" --own-pids 79; expect "another's load: a warning, with its pid" 0 'warn +pi +other load: stress 99 % \(pid 77\) of a core'
refuse "hangar's own load, this session's hellion and its relay are not another's" 'hangar-mux|hellion 60|mavros_node'

# Ports: the camera's stream held by a hellion that is not this session's (finding F1).
facts "$T/ports.json" 'f["ports"] = [{"proto": "udp", "port": 5610, "held": True, "comm": "hellion", "pid": 812},
    {"proto": "tcp", "port": 8080, "held": True, "comm": "nginx", "pid": 300}, {"proto": "udp", "port": 14560, "held": False, "comm": "", "pid": 0},
    {"proto": "udp", "port": 5614, "held": True, "comm": "hangar-bridge", "pid": 9}, {"proto": "udp", "port": 14650, "held": True, "comm": "?", "pid": 0}]'
judge up "$T/ports.json"
expect "ports taken" 1 'FAIL +pi +UDP 5610: taken by hellion \(pid 812\)' 'sudo systemctl stop hellion' 'warn +pi +TCP 8080: taken by nginx' \
    'ok +pi +UDP 14560: free' 'ok +pi +UDP 5614: held by this session.s own hangar-bridge' 'FAIL +pi +UDP 14650: taken by a process of another user'
facts "$T/relay.json" 'f["ports"] = [{"proto": "udp", "port": 14555, "held": True, "comm": "mavros_node", "pid": 51, "own": True}]'
judge up "$T/relay.json"; expect "the session's own relay holds its port (its pid file's group)" 0 'ok +pi +UDP 14555: held by this session.s own mavros_node'
facts "$T/relay2.json" 'f["ports"] = [{"proto": "udp", "port": 14555, "held": True, "comm": "mavros_node", "pid": 51, "own": False}]'
judge up "$T/relay2.json"; expect "a MAVROS that is not the session's holds the relay's port" 1 'FAIL +pi +UDP 14555: taken by mavros_node'
# hangar's modules are not dumpable: the kernel names no holder for their sockets. With
# a manager of the session running there, such a port is the session's (up on a session
# that is up); without one it is another's.
facts "$T/hidden.json" 'f["ports"] = [{"proto": "udp", "port": 14560, "held": True, "comm": "?", "pid": 0}]; f["hangar_running"] = ["/dev/shm/hangar-vehicle"]'
judge up "$T/hidden.json"; expect "a port held by a module of the running session" 0 'ok +pi +UDP 14560: held, by a process that does not show its sockets: hangar runs here'
judge up "$T/ports.json" --own-pids 812,300
expect "this session's own hellion holds its port" 1 'ok +pi +UDP 5610: held by this session.s own hellion'

facts "$T/svc.json" 'f["services"]["eeye"] = "active"'
judge measure "$T/svc.json"; expect "the lab's own service beside a measurement: a warning" 0 'warn +pi +service eeye: active'
judge up "$T/svc.json";      expect "the lab's own service before a session: said" 0 'ok +pi +service eeye: active'

facts "$T/nif.json" 'f["not_in_force"] = ["safety: asks fifo 60, runs other, nice 0; asks locked memory, has none locked (the manager is not root)"]; f["rt"]["caps"] = ""'
judge measure "$T/nif.json"; expect "a [Schedule] not in force: a FAIL before a measurement" 1 'FAIL +pi +schedule: not in force: safety: asks fifo 60' 'warn +pi +real time: not reachable' 'lab.sh deploy --realtime'
judge up "$T/nif.json";      expect "a [Schedule] not in force: a warning before a session" 0 'warn +pi +schedule: not in force'
facts "$T/nosuid.json" 'f["rt"]["nosuid"] = True'
judge up "$T/nosuid.json"; expect "capabilities on a nosuid filesystem" 0 'warn +pi +real time: hangard has cap_ipc_lock,cap_sys_nice=ep, but its filesystem is mounted nosuid'
facts "$T/limit.json" 'f["rt"]["caps"] = ""; f["rt"]["rtprio"] = 70'
judge measure "$T/limit.json"; expect "real time by the limit" 0 'ok +pi +real time: reachable by RLIMIT_RTPRIO 70'

facts "$T/rel-l.json" 'f["host"] = "mousev"; f["release"]["commit"] = "b" * 40; f["release"]["version"] = "v0.1.5"; f["rt"]["hangard"] = False'
judge up "$T/clean.json" --laptop "$T/rel-l.json"; expect "two releases" 0 'warn +both +release: the Pi has v0.1.6 \(aaaaaaa\), this laptop v0.1.5 \(bbbbbbb\)'

judge up "$T/none.json"; expect "no facts from the Pi" 1 'FAIL +pi +probe: no facts from this machine'

echo "== the probe, on this machine"
# A process of the package system and Ubuntu's idle waiter: two copies of sleep under
# their names (the kernel's name for both is the first 15 characters).
cp "$(command -v sleep)" "$T/unattended-upgr"
cp "$(command -v sleep)" "$T/unattended-upgrade-shutdown"
"$T/unattended-upgr" 60 & pids+=($!); run_pid=$!
"$T/unattended-upgrade-shutdown" 60 & pids+=($!); idle_pid=$!
# And a UDP port held by a process of this test.
port=$(python3 -I -c 'import socket; s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
python3 -I -c 'import socket, sys, time; s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", int(sys.argv[1]))); time.sleep(60)' "$port" &
pids+=($!); hold_pid=$!
sleep 0.5
# A port held by a process a pid file of the session names: its own.
python3 -I -c 'import socket, sys, time; s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1], flush=True); time.sleep(60)' > "$T/own.port" &
pids+=($!); own_pid=$!
echo "$own_pid" > "$T/own.pid"
sleep 0.5
own_port=$(cat "$T/own.port")
if python3 -I - probe --ports "udp:$port,tcp:1,udp:$own_port" --services "no-such-unit-of-hangar" --root "$T/no-install" --hangar-root "$T/no-root" \
        --own-pidfiles "$T/none.pid,$T/own.pid" \
        < "$PF" > "$T/here.json" 2> "$T/here.err"; then
    ok "the probe runs from standard input"
else
    bad "the probe failed: $(cat "$T/here.err")"
fi
check=$(python3 -I - "$T/here.json" "$run_pid" "$idle_pid" "$hold_pid" "$port" "$own_port" <<'EOF'
import json, shutil, sys
f = json.load(open(sys.argv[1]))
run, idle, hold, port, own_port = (int(a) for a in sys.argv[2:7])
keys = ["hangar_running", "host", "uptime_s", "busy", "apt", "timers", "temp_c", "throttled", "governor", "mem_avail_mb", "shm_avail_mb",
        "home_avail_mb", "ports", "services", "release", "rt", "not_in_force", "now"]
print("keys", "ok" if all(k in f for k in keys) else "missing " + ",".join(k for k in keys if k not in f))
apt = {p["pid"] for p in f["apt"]}
print("apt-found", "ok" if run in apt else "no: %r" % f["apt"])
print("idle-waiter", "ok" if idle not in apt else "taken for a run")
p = {x["port"]: x for x in f["ports"]}
if shutil.which("ss"):
    print("port-held", "ok" if p[port]["held"] and p[port]["pid"] == hold else "no: %r" % p[port])
    print("port-free", "ok" if not p[1]["held"] else "no: %r" % p[1])
    print("port-not-own", "ok" if not p[port]["own"] else "no: %r" % p[port])
    print("port-own-by-pid-file", "ok" if p[own_port]["held"] and p[own_port]["own"] else "no: %r" % p[own_port])
else:
    print("port-held", "skip"); print("port-free", "skip"); print("port-not-own", "skip"); print("port-own-by-pid-file", "skip")
print("no-install", "ok" if not f["rt"]["hangard"] and f["release"]["commit"] is None else "no")
print("room", "ok" if f["mem_avail_mb"] and f["shm_avail_mb"] is not None else "no")
EOF
)
while read -r what verdict; do
    case $verdict in
        ok) ok "the probe: $what" ;;
        skip) echo "  skip  the probe: $what (no ss on this machine)" ;;
        *) bad "the probe: $what: $verdict" ;;
    esac
done <<< "$check"
judge measure "$T/here.json"
if [ "$rc" = 1 ] && grep -Eq "FAIL +pi +package system: running now: unattended-upgr \(pid $run_pid\)" <<< "$out"; then
    ok "the judge over this machine's facts names the package system's process"
else
    bad "the judge over this machine's facts (status $rc)"; echo "$out" | sed 's/^/        /'
fi
kill "$run_pid" "$idle_pid" "$hold_pid" "$own_pid" 2>/dev/null

echo "== lab.sh preflight, rehearsing"
out=$(LAB_HOST=local ROV_ROOT=$T/no-install LAB_STATE_DIR=$T/state RESULTS_DIR=$T/results HOME=$T/home \
      bash "$here/../lab.sh" preflight 2>&1); rc=$?
if grep -Eq '^preflight \(up\): [0-9]+ ok, [0-9]+ warn, [0-9]+ FAIL$' <<< "$out" && grep -Eq '(ok|warn|FAIL) +pi +(UDP 14560|TCP 8080)' <<< "$out"; then
    ok "lab.sh preflight reports, with the session's ports (status $rc)"
else
    bad "lab.sh preflight (status $rc)"; echo "$out" | sed 's/^/        /'
fi
out=$(LAB_HOST=local ROV_ROOT=$T/no-install LAB_STATE_DIR=$T/state RESULTS_DIR=$T/results HOME=$T/home \
      bash "$here/../lab.sh" preflight --measure 2>&1)
if grep -Eq '^preflight \(measure\): ' <<< "$out" && ! grep -Eq 'pi +(UDP|TCP) ' <<< "$out"; then
    ok "lab.sh preflight --measure reports, without ports"
else
    bad "lab.sh preflight --measure"; echo "$out" | sed 's/^/        /'
fi

echo
if [ "$fails" = 0 ]; then echo "preflight: $n checks ok"; else echo "preflight: $fails of $n checks FAILED"; exit 1; fi
