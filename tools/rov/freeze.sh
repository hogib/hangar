#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/rov/freeze.sh: the Pi held still for a session of measurements, and put back
# (tools/rov/README.md, "Freeze and thaw"). tools/rov/lab.sh sends this file to the Pi
# and runs it there: `plan` and `status` as the Pi's user, `freeze` and `thaw` as root
# under one sudo.
#
#   freeze.sh plan   [NAME=value ...]   what freeze would do here, command by command;
#                                       nothing is changed
#   freeze.sh freeze [NAME=value ...]   the state as found written down, then: the
#                                       package system's timers and units kept from
#                                       starting until the next boot, a running upgrade
#                                       asked to end and waited for, the lab's own
#                                       services stopped, the governor set, the clock set
#   freeze.sh thaw   [NAME=value ...]   everything put back as written down; run twice,
#                                       the second finds nothing to do
#   freeze.sh status [NAME=value ...]   frozen or not, and what was written down
#
# Settings, as NAME=value arguments (sudo hands on no environment):
#   TIMERS    units masked for this boot and stopped   (apt-daily.timer apt-daily-upgrade.timer)
#   UNITS     units masked for this boot, never stopped by systemctl: what runs in one
#             is asked to end by its own signal        (apt-daily.service apt-daily-upgrade.service)
#   SERVICES  the lab's own, stopped; thaw starts those that ran   (none)
#   GOVERNOR  the cpufreq governor                     (performance)
#   CLOCK_OURS, CLOCK_THEIRS   the laptop's time and this machine's, both read at the
#             same moment (seconds since 1970): the clock is set by their difference if
#             it is more than 2 s                      (not set)
#   WAIT      seconds to wait for the package system to end (900)
#
# What it never does: SIGKILL anything; interrupt dpkg (a package half installed is
# worse than a late measurement: it waits, and says so); mask a unit for good. The
# masks are systemd's runtime ones, under /run, and the record is /run/hangar-frozen:
# a reboot undoes the freeze by itself, record and all, and a thaw that was forgotten
# costs the lab's Pi no update.
#
# For tests and the rehearsal, every command that changes the machine can be replaced:
# STATE (the record), SYSTEMCTL, KILL, SLEEP, DATE (commands), PROC and CPU (/proc and
# /sys/devices/system/cpu). With any of them given the script does not ask to be root.
set -uo pipefail

cmd=${1:-}
[ $# -gt 0 ] && shift

TIMERS="apt-daily.timer apt-daily-upgrade.timer"
UNITS="apt-daily.service apt-daily-upgrade.service"
SERVICES=""
GOVERNOR=performance
CLOCK_OURS="" CLOCK_THEIRS=""
WAIT=900
STATE=/run/hangar-frozen
SYSTEMCTL=systemctl KILL=kill SLEEP=sleep DATE=date
PROC=/proc
CPU=/sys/devices/system/cpu
seam=0
for a in "$@"; do
    case $a in
        TIMERS=*|UNITS=*|SERVICES=*|GOVERNOR=*|CLOCK_OURS=*|CLOCK_THEIRS=*|WAIT=*) declare "$a" ;;
        STATE=*|SYSTEMCTL=*|KILL=*|SLEEP=*|DATE=*|PROC=*|CPU=*) declare "$a"; seam=1 ;;
        *) echo "freeze: unknown setting $a" >&2; exit 2 ;;
    esac
done
read -ra systemctl <<< "$SYSTEMCTL"
read -ra kill_ <<< "$KILL"
read -ra sleep_ <<< "$SLEEP"
read -ra date_ <<< "$DATE"

say() { echo "freeze: $*"; }
warn() { echo "freeze: $*" >&2; }
# Every change goes through here: said first, as the command it is.
run() { echo "  # $*"; "$@" < /dev/null; }

need_root() {
    [ "$seam" = 1 ] || [ "$(id -u)" = 0 ] || { warn "$cmd changes the system: it runs as root (lab.sh $cmd asks for sudo once)"; exit 1; }
}

is_active() { "${systemctl[@]}" is-active "$1" 2>/dev/null | head -1; }
load_state() { "${systemctl[@]}" show -p LoadState --value "$1" 2>/dev/null | head -1; }
gov_files() { ls "$CPU"/cpu[0-9]*/cpufreq/scaling_governor 2>/dev/null; }

# The package system's processes: "pid comm" a line. Ubuntu's idle waiter
# (unattended-upgrade-shutdown, there from boot to shutdown) is not one: the kernel's
# name for it is the same 15 characters as the upgrade's, so the command line decides.
pkg_procs() {
    local d comm
    for d in "$PROC"/[0-9]*; do
        [ -r "$d/comm" ] || continue
        comm=$(cat "$d/comm" 2>/dev/null) || continue
        case $comm in
            unattended-upgr|apt|apt-get|apt.systemd.dai|dpkg|apt-check) ;;
            *) continue ;;
        esac
        tr '\0' ' ' < "$d/cmdline" 2>/dev/null | grep -q 'unattended-upgrade-shutdown' && continue
        echo "${d##*/} $comm"
    done
}

rec_get() { [ -f "$STATE" ] && awk -v k="$1" '$1 == k { $1 = ""; sub(/^ /, ""); print }' "$STATE"; }
rec_add() { echo "$*" >> "$STATE"; }

frozen_since() { rec_get frozen_at | head -1; }

# What freeze would do, from what is here now. Shared by plan and freeze, so that the
# plan printed is the plan carried out.
todo_mask=() todo_stop=() todo_gov=()
survey() {
    local u g
    todo_mask=() todo_stop=() todo_gov=()
    for u in $TIMERS $UNITS; do
        [ "$(load_state "$u")" = loaded ] && todo_mask+=("$u")
    done
    for u in $SERVICES; do
        [ "$(is_active "$u")" = active ] && todo_stop+=("$u")
    done
    for g in $(gov_files); do
        [ "$(cat "$g")" = "$GOVERNOR" ] || todo_gov+=("$g")
    done
}

clock_skew() { # prints the seconds to add to this clock, if more than 2 either way
    [ -n "$CLOCK_OURS" ] && [ -n "$CLOCK_THEIRS" ] || return 1
    local d=$((CLOCK_OURS - CLOCK_THEIRS))
    [ "${d#-}" -gt 2 ] || return 1
    echo "$d"
}

cmd_plan() {
    local u g d procs
    if [ -f "$STATE" ]; then say "already frozen, since $(frozen_since): freeze would go over the same steps and keep the record"; fi
    survey
    say "as root, on $(hostname):"
    echo "  # the state as found written to $STATE (under /run: gone with the next boot)"
    for u in ${todo_mask[@]+"${todo_mask[@]}"}; do
        case " $TIMERS " in
            *" $u "*) echo "  # $SYSTEMCTL mask --runtime --now $u" ;;
            *) echo "  # $SYSTEMCTL mask --runtime $u" ;;
        esac
    done
    for u in $TIMERS $UNITS; do
        [[ " ${todo_mask[*]-} " == *" $u "* ]] || echo "  # $u: $(load_state "$u" | sed 's/^$/not known/') here, left alone"
    done
    procs=$(pkg_procs)
    if [ -n "$procs" ]; then
        echo "  # the package system runs now ($(echo "$procs" | awk '{ printf "%s%s (pid %s)", s, $2, $1; s = ", " }')):"
        echo "  #   kill -TERM to unattended-upgrade alone (it ends after the package it is at), then"
        echo "  #   wait up to $WAIT s for all of it; dpkg is never interrupted"
    else
        echo "  # the package system does not run now: nothing to end"
    fi
    [ "${#todo_stop[@]}" -gt 0 ] && echo "  # $SYSTEMCTL stop ${todo_stop[*]}"
    for u in $SERVICES; do
        [[ " ${todo_stop[*]-} " == *" $u "* ]] || echo "  # $u: $(is_active "$u" | sed 's/^$/not known/'), left alone"
    done
    for g in ${todo_gov[@]+"${todo_gov[@]}"}; do echo "  # echo $GOVERNOR > $g   (now $(cat "$g"))"; done
    [ -n "$(gov_files)" ] || echo "  # no cpufreq governor here: none set"
    if d=$(clock_skew); then echo "  # $DATE -u -s @<now + $d s>   (the clock is $d s off the laptop's)"
    elif [ -n "$CLOCK_OURS" ]; then echo "  # the clock is within 2 s of the laptop's: not set"; fi
}

cmd_freeze() {
    need_root
    local u g d procs t pid comm left
    survey
    if [ -f "$STATE" ]; then
        say "already frozen, since $(frozen_since): the record is kept, the steps gone over again"
    else
        # The record first, whole, before anything is changed: a freeze interrupted
        # half-way is thawed from it.
        local tmp=$STATE.tmp
        {
            echo "frozen_at $("${date_[@]}" -u '+%Y-%m-%dT%H:%M:%SZ')"
            for u in $TIMERS; do echo "timer $u $(is_active "$u" | sed 's/^$/unknown/') $(load_state "$u" | sed 's/^$/unknown/')"; done
            for u in $UNITS; do echo "unit $u $(is_active "$u" | sed 's/^$/unknown/') $(load_state "$u" | sed 's/^$/unknown/')"; done
            for u in $SERVICES; do echo "service $u $(is_active "$u" | sed 's/^$/unknown/')"; done
            for g in $(gov_files); do echo "governor $g $(cat "$g")"; done
        } > "$tmp" && mv "$tmp" "$STATE" || { warn "cannot write $STATE: nothing was changed"; exit 1; }
        chmod 644 "$STATE"
        say "the state as found is written to $STATE"
    fi

    # Nothing new of the package system starts from here on, until the next boot.
    for u in ${todo_mask[@]+"${todo_mask[@]}"}; do
        grep -qx "masked $u" "$STATE" || rec_add "masked $u"
        case " $TIMERS " in
            *" $u "*) run "${systemctl[@]}" mask --runtime --now "$u" ;;
            *) run "${systemctl[@]}" mask --runtime "$u" ;;
        esac || warn "could not mask $u: it may still start by itself (lab.sh preflight says)"
    done

    # What of it runs now: unattended-upgrade is asked to end, which it does after the
    # package it is at; everything else of it is waited for.
    procs=$(pkg_procs)
    if [ -n "$procs" ]; then
        while read -r pid comm; do
            [ "$comm" = unattended-upgr ] && run "${kill_[@]}" -TERM "$pid"
        done <<< "$procs"
        t=0
        while left=$(pkg_procs) && [ -n "$left" ]; do
            if [ "$t" -ge "$WAIT" ]; then
                warn "the package system still runs after $WAIT s: $(echo "$left" | awk '{ printf "%s%s (pid %s)", s, $2, $1; s = ", " }')"
                warn "  It was not interrupted. The rest of the freeze goes on; a measurement must wait"
                warn "  for it (lab.sh preflight --measure says when it has ended)."
                rc_pkg=3
                break
            fi
            if [ $((t % 30)) = 0 ]; then
                if grep -q ' dpkg$' <<< "$left"; then say "dpkg is installing a package: waiting for it, not interrupting ($t s so far)"
                else say "waiting for the package system to end: $(echo "$left" | awk '{ printf "%s%s", s, $2; s = ", " }') ($t s so far)"; fi
            fi
            "${sleep_[@]}" 5
            t=$((t + 5))
        done
        [ -n "${left:-}" ] || say "the package system has ended (after $t s)"
    else
        say "the package system does not run"
    fi

    for u in ${todo_stop[@]+"${todo_stop[@]}"}; do
        grep -qx "stopped $u" "$STATE" || rec_add "stopped $u"
    done
    if [ "${#todo_stop[@]}" -gt 0 ]; then
        run "${systemctl[@]}" stop "${todo_stop[@]}" || warn "could not stop ${todo_stop[*]}"
    fi
    for g in ${todo_gov[@]+"${todo_gov[@]}"}; do
        echo "  # echo $GOVERNOR > $g"
        echo "$GOVERNOR" > "$g" || warn "could not set the governor in $g"
    done
    if d=$(clock_skew); then
        say "the clock is $d s off the laptop's: setting it"
        # The time the password and the steps above took is in this clock too: it has
        # counted since CLOCK_THEIRS was read.
        local to; to=$(( CLOCK_OURS + $("${date_[@]}" -u +%s) - CLOCK_THEIRS ))
        echo "  # $DATE -u -s @$to"
        "${date_[@]}" -u -s "@$to" > /dev/null || warn "could not set the clock"
    fi
    say "frozen. Until lab.sh thaw, or the next boot, whichever comes first"
    exit "${rc_pkg:-0}"
}

cmd_thaw() {
    need_root
    [ -f "$STATE" ] || { say "not frozen: nothing to put back"; exit 0; }
    local kind name was rest failed=0 g v
    say "putting back what was written down at $(frozen_since)"
    # The masks first: a masked unit cannot be started.
    while read -r name; do
        [ -n "$name" ] && { run "${systemctl[@]}" unmask --runtime "$name" || failed=1; }
    done <<< "$(rec_get masked)"
    while read -r kind name was rest; do
        case $kind in
            timer)
                if [ "$was" = active ] && [ "$(is_active "$name")" != active ]; then
                    run "${systemctl[@]}" start "$name" || failed=1
                else
                    echo "  # $name was ${was:-unknown} and is $(is_active "$name" | sed 's/^$/unknown/'): left"
                fi ;;
            unit)
                # A unit of the package system that ran when the Pi was frozen is not
                # started again: its timer is, and runs it when it is due.
                echo "  # $name was ${was:-unknown}: left to its timer" ;;
            service)
                if grep -qx "stopped $name" "$STATE" && [ "$(is_active "$name")" != active ]; then
                    run "${systemctl[@]}" start "$name" || failed=1
                else
                    echo "  # $name was ${was:-unknown} and is $(is_active "$name" | sed 's/^$/unknown/'): left"
                fi ;;
            governor)
                g=$name v=$was
                if [ -w "$g" ] && [ "$(cat "$g")" != "$v" ]; then
                    echo "  # echo $v > $g"
                    echo "$v" > "$g" || failed=1
                fi ;;
        esac
    done < "$STATE"
    if [ "$failed" = 1 ]; then
        warn "not everything could be put back: $STATE is kept, run thaw again (a reboot undoes the masks and the governor by itself)"
        exit 1
    fi
    rm -f "$STATE"
    say "thawed: the timers, the lab's services and the governor are as they were found; the clock stays as set"
}

cmd_status() {
    if [ -f "$STATE" ]; then
        echo "frozen since $(frozen_since)"
        sed 's/^/  /' "$STATE"
    else
        echo "not frozen"
    fi
}

case $cmd in
    plan) cmd_plan ;;
    freeze) cmd_freeze ;;
    thaw) cmd_thaw ;;
    status) cmd_status ;;
    *) sed -n '3,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
