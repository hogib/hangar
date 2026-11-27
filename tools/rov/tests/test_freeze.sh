#!/usr/bin/env bash
# tools/rov/tests/test_freeze.sh: freeze and thaw (tools/rov/README.md, "Freeze and
# thaw"), without sudo and without the Pi: `make check-rov`.
#
# freeze.sh runs against a Pi that is a directory (tests/fake-pi.sh): its systemctl,
# kill, sleep and date are stand-ins that write down what they were asked, its /proc
# and its governors are files. The stand-in kill refuses every signal but SIGTERM, so a
# SIGKILL anywhere fails the run it is in. What this does not show is the real
# systemctl's and the real unattended-upgrade's behaviour: that is the lab's to show
# (LAB-CHECKLIST.md, step 1).
set -uo pipefail

here=$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")
FZ=$here/../freeze.sh
FAKE=$here/fake-pi.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/hangar-freeze.XXXXXX")
trap 'rm -rf "$T"' EXIT

fails=0 n=0
ok() { n=$((n + 1)); echo "  ok    $*"; }
bad() { n=$((n + 1)); fails=$((fails + 1)); echo "  FAIL  $*"; }
is() { # name command...
    local name=$1; shift
    if "$@"; then ok "$name"; else bad "$name"; [ -z "${out:-}" ] || echo "$out" | sed 's/^/        /'; fi
}

# pi NAME [unit=state ...]: a new Pi in $P, its seams in S.
pi() {
    P=$T/$1; shift
    bash "$FAKE" make "$P" "$@"
    readarray -t S < <(bash "$FAKE" seams "$P")
}
fz() { out=$(bash "$FZ" "$@" SERVICES="eeye hellion" "${S[@]}" 2>&1); rc=$?; }
unit() { cat "$P/units/$1"; }
gov() { sort -u "$P"/cpu/cpu*/cpufreq/scaling_governor | tr '\n' ' '; }
proc() { # pid comm cmdline [file=value]
    mkdir -p "$P/proc/$1"; echo "$2" > "$P/proc/$1/comm"; printf '%s\0' $3 > "$P/proc/$1/cmdline"
    [ -z "${4:-}" ] || echo "${4#*=}" > "$P/proc/$1/${4%%=*}"
}
logged() { grep -Eq -- "$1" "$P/log"; }

echo "== plan"
pi plan apt-daily.timer=inactive
before=$(find "$P" -type f ! -name log -exec sha256sum {} + | sort)
fz plan CLOCK_OURS=5000 CLOCK_THEIRS=1000
is "plan says each command" eval 'grep -q "mask --runtime --now apt-daily-upgrade.timer" <<< "$out" && grep -q "stop eeye hellion" <<< "$out" &&
    grep -q "echo performance > .*cpu0/cpufreq/scaling_governor   (now ondemand)" <<< "$out" && grep -q "4000 s off" <<< "$out"'
is "plan changes nothing" [ "$before" = "$(find "$P" -type f ! -name log -exec sha256sum {} + | sort)" ]
is "plan runs nothing that changes: no mask, stop, kill or date -s in the log" eval '! logged "^(systemctl (mask|unmask|stop|start)|kill|date .*-s)"'

echo "== freeze and thaw, one timer inactive as found, the governor ondemand"
fz freeze CLOCK_OURS=5000 CLOCK_THEIRS=1000
is "freeze: status 0" [ "$rc" = 0 ]
is "the record: every unit's state as found, and the governors" eval 'grep -qx "timer apt-daily.timer inactive loaded" "$P/run/hangar-frozen" &&
    grep -qx "timer apt-daily-upgrade.timer active loaded" "$P/run/hangar-frozen" && grep -qx "service eeye active" "$P/run/hangar-frozen" &&
    [ "$(grep -c "^governor .* ondemand$" "$P/run/hangar-frozen")" = 4 ]'
is "both timers and both units masked for this boot only (--runtime)" eval '[ -f "$P/units/apt-daily.timer.rtmask" ] && [ -f "$P/units/apt-daily-upgrade.timer.rtmask" ] &&
    [ -f "$P/units/apt-daily.service.rtmask" ] && [ -f "$P/units/apt-daily-upgrade.service.rtmask" ] && ! logged "systemctl mask [^-]" && ! grep "systemctl mask" "$P/log" | grep -qv -- "--runtime"'
is "the timers stopped, the lab's services stopped" eval '[ "$(unit apt-daily-upgrade.timer)" = inactive ] && [ "$(unit eeye)" = inactive ] && [ "$(unit hellion)" = inactive ]'
is "the governor: performance on every CPU" [ "$(gov)" = "performance " ]
is "the clock set, by the difference" logged '^date -u -s @[0-9]+$'
is "nothing was signalled: nothing of the package system ran" eval '! logged "^kill"'
fz status
is "status: frozen, with the record" eval 'grep -q "^frozen since 20" <<< "$out" && grep -q "masked apt-daily.timer" <<< "$out"'

fz freeze
is "freeze again: the first record is kept (the governors as found, not as set)" eval '[ "$rc" = 0 ] && grep -q "already frozen" <<< "$out" &&
    [ "$(grep -c "^governor .* ondemand$" "$P/run/hangar-frozen")" = 4 ] && [ "$(grep -c "^frozen_at" "$P/run/hangar-frozen")" = 1 ]'

fz thaw
is "thaw: status 0, the record gone" eval '[ "$rc" = 0 ] && [ ! -e "$P/run/hangar-frozen" ]'
is "the timer that ran runs again; the one that did not is left as it was" eval '[ "$(unit apt-daily-upgrade.timer)" = active ] && [ "$(unit apt-daily.timer)" = inactive ]'
is "no mask left" eval '! ls "$P"/units/*.rtmask > /dev/null 2>&1'
is "the lab's services started again" eval '[ "$(unit eeye)" = active ] && [ "$(unit hellion)" = active ]'
is "the governor as found" [ "$(gov)" = "ondemand " ]
is "the units of the package system are not started by thaw" eval '! logged "systemctl start apt-daily(-upgrade)?.service"'
: > "$P/log"
fz thaw
is "thaw twice: nothing to put back, status 0, nothing run" eval '[ "$rc" = 0 ] && grep -q "not frozen" <<< "$out" && [ ! -s "$P/log" ]'

echo "== what was not running is not started, what is not there is left alone"
pi partial hellion=inactive
rm "$P/units/apt-daily.service"
fz freeze
is "a unit that is not on this machine: not masked, and said" eval '[ "$rc" = 0 ] && ! logged "mask .*apt-daily.service" && grep -qx "unit apt-daily.service inactive not-found" "$P/run/hangar-frozen"'
fz thaw
is "a service that did not run when frozen is not started by thaw" eval '[ "$(unit hellion)" = inactive ] && [ "$(unit eeye)" = active ]'

echo "== an upgrade running, which takes a while to end"
pi upgrade
proc 4242 unattended-upgr "/usr/bin/python3 /usr/bin/unattended-upgrade" ends_after_term=3
proc 800 unattended-upgr "/usr/bin/python3 /usr/share/unattended-upgrades/unattended-upgrade-shutdown --wait-for-signal"
proc 4300 apt.systemd.dai "/bin/sh /usr/lib/apt/apt.systemd.daily install" ends_in=4
fz freeze
is "freeze waits for it and ends 0" eval '[ "$rc" = 0 ] && grep -q "the package system has ended" <<< "$out"'
is "SIGTERM to unattended-upgrade, once, and to nothing else" eval '[ "$(grep -c "^kill" "$P/log")" = 1 ] && logged "^kill -TERM 4242$"'
is "it was waited for: the stand-in sleep ran, and both processes are gone" eval 'logged "^sleep 5$" && [ ! -d "$P/proc/4242" ] && [ ! -d "$P/proc/4300" ]'
is "Ubuntu's idle waiter is left alone" [ -d "$P/proc/800" ]
is "the masks went on before the wait: nothing new starts meanwhile" eval '[ "$(grep -n "mask --runtime" "$P/log" | tail -1 | cut -d: -f1)" -lt "$(grep -n "^kill" "$P/log" | cut -d: -f1)" ]'

echo "== dpkg at work"
pi dpkg
proc 4242 unattended-upgr "/usr/bin/python3 /usr/bin/unattended-upgrade" ends_after_term=8
proc 4250 dpkg "/usr/bin/dpkg --configure -a" ends_in=6
fz freeze
is "dpkg is waited for, and said" eval '[ "$rc" = 0 ] && grep -q "dpkg is installing a package: waiting for it, not interrupting" <<< "$out"'
is "dpkg is never signalled" eval '! logged "^kill .* 4250"'

echo "== a package system that does not end"
pi stuck
proc 4250 dpkg "/usr/bin/dpkg --unpack big.deb"
fz freeze WAIT=20
is "after WAIT: status 3, said, nothing killed" eval '[ "$rc" = 3 ] && grep -q "still runs after 20 s: dpkg (pid 4250)" <<< "$out" && ! logged "^kill" && [ -d "$P/proc/4250" ]'
is "the rest of the freeze was done all the same" eval '[ "$(gov)" = "performance " ] && [ "$(unit eeye)" = inactive ] && [ -f "$P/run/hangar-frozen" ]'
fz thaw
is "and thaws as any other" eval '[ "$rc" = 0 ] && [ "$(gov)" = "ondemand " ] && [ "$(unit eeye)" = active ]'

echo "== a freeze that stopped half-way"
pi half
# The record whole, and one mask on: as after a freeze interrupted at its second step.
fz freeze; cp "$P/run/hangar-frozen" "$T/rec"; fz thaw
bash "$FAKE" make "$P"
mkdir -p "$P/run"; grep -vE '^(masked|stopped) ' "$T/rec" > "$P/run/hangar-frozen"; echo "masked apt-daily.timer" >> "$P/run/hangar-frozen"
: > "$P/units/apt-daily.timer.rtmask"; echo inactive > "$P/units/apt-daily.timer"
fz thaw
is "thaw puts back what had been done" eval '[ "$rc" = 0 ] && [ ! -f "$P/units/apt-daily.timer.rtmask" ] && [ "$(unit apt-daily.timer)" = active ]'

echo "== without the stand-ins"
if [ "$(id -u)" != 0 ]; then
    out=$(bash "$FZ" freeze 2>&1); rc=$?
    is "freeze as an ordinary user: refused before anything" eval '[ "$rc" = 1 ] && grep -q "runs as root" <<< "$out"'
    out=$(bash "$FZ" thaw 2>&1); rc=$?
    is "thaw as an ordinary user: refused" eval '[ "$rc" = 1 ] && grep -q "runs as root" <<< "$out"'
else
    echo "  skip  the refusal of an ordinary user: this test runs as root, and would freeze this machine"
fi
out=$(bash "$FZ" freeze NOSUCH=1 2>&1); rc=$?
is "an unknown setting: refused" [ "$rc" = 2 ]

echo "== lab.sh freeze and thaw, rehearsing (LAB_HOST=local: a Pi that is a directory)"
lab() { out=$(LAB_HOST=local ROV_ROOT=$T/no-install LAB_STATE_DIR=$T/state RESULTS_DIR=$T/results HOME=$T/home bash "$here/../lab.sh" "$@" 2>&1); rc=$?; }
mkdir -p "$T/home"
lab preflight --measure
is "before: the preflight refuses a measurement, and names the command" eval 'grep -Eq "FAIL +pi +frozen: not frozen" <<< "$out" && grep -q "lab.sh freeze" <<< "$out"'
lab freeze
is "lab.sh freeze: the commands shown, then done, the record kept on both sides" eval '[ "$rc" = 0 ] && grep -q "mask --runtime --now apt-daily.timer" <<< "$out" &&
    grep -q "frozen. Until lab.sh thaw" <<< "$out" && [ -s "$T/state/frozen" ] && [ -s "$T/state/freeze-rehearsal/run/hangar-frozen" ]'
lab preflight --measure
is "after: the preflight reads frozen" eval 'grep -Eq "ok +pi +frozen: since 20" <<< "$out"'
lab status
is "lab.sh status says the Pi is frozen" eval 'grep -q "THE PI IS FROZEN" <<< "$out"'
lab down
is "lab.sh down says so too, and does not say everything is as found" eval 'grep -q "THE PI IS FROZEN" <<< "$out" && ! grep -q "lab: everything as found" <<< "$out"'
lab thaw
is "lab.sh thaw: put back, both records gone" eval '[ "$rc" = 0 ] && grep -q "thawed" <<< "$out" && [ ! -e "$T/state/frozen" ] && [ ! -e "$T/state/freeze-rehearsal/run/hangar-frozen" ]'
lab thaw
is "lab.sh thaw again: nothing to put back, status 0" eval '[ "$rc" = 0 ] && grep -q "not frozen" <<< "$out"'
lab status
is "status is quiet about it again" eval '! grep -q "FROZEN" <<< "$out"'

if [ "$fails" = 0 ]; then echo "freeze: $n checks ok"; else echo "freeze: $fails of $n checks FAILED"; exit 1; fi
