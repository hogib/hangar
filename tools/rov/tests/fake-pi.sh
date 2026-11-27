#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# tools/rov/tests/fake-pi.sh: stand-ins for what tools/rov/freeze.sh changes on the Pi,
# for its tests (test_freeze.sh) and for lab.sh's rehearsal (LAB_HOST=local), where
# nothing of the laptop may be touched. The Pi it plays is a directory, $FAKE_PI:
#
#   units/<unit>            "active" or "inactive" (no file: the unit is not there)
#   units/<unit>.rtmask     the unit is masked for this boot
#   proc/<pid>/comm, cmdline  a process; with proc/<pid>/ends_in N it ends by itself
#                           after N sleeps, with ends_after_term N, N sleeps after it
#                           was sent SIGTERM (proc/<pid>/termed)
#   cpu/cpu<N>/cpufreq/scaling_governor   plain files: freeze.sh writes them itself
#   log                     every command, a line each
#
#   fake-pi.sh make DIR [timer=state ...]   lay one out: two apt timers and their
#                           units, eeye and hellion, four CPUs under ondemand
#   fake-pi.sh seams DIR                    the NAME=value arguments that point
#                           freeze.sh at it
#   fake-pi.sh systemctl|kill|sleep|date ...   the stand-ins themselves
set -uo pipefail
self=$(realpath -- "${BASH_SOURCE[0]}")
what=${1:-}; [ $# -gt 0 ] && shift

if [ "$what" = make ]; then
    d=$1; shift
    mkdir -p "$d/units" "$d/proc" "$d/run"
    for u in apt-daily.timer apt-daily-upgrade.timer eeye hellion; do echo active > "$d/units/$u"; done
    for u in apt-daily.service apt-daily-upgrade.service; do echo inactive > "$d/units/$u"; done
    for c in 0 1 2 3; do mkdir -p "$d/cpu/cpu$c/cpufreq"; echo ondemand > "$d/cpu/cpu$c/cpufreq/scaling_governor"; done
    for kv in "$@"; do echo "${kv#*=}" > "$d/units/${kv%%=*}"; done
    : > "$d/log"
    exit 0
fi
if [ "$what" = seams ]; then
    d=$(realpath -- "$1")
    printf '%s\n' "STATE=$d/run/hangar-frozen" "SYSTEMCTL=env FAKE_PI=$d bash $self systemctl" "KILL=env FAKE_PI=$d bash $self kill" \
        "SLEEP=env FAKE_PI=$d bash $self sleep" "DATE=env FAKE_PI=$d bash $self date" "PROC=$d/proc" "CPU=$d/cpu"
    exit 0
fi

: "${FAKE_PI:?fake-pi.sh: FAKE_PI is not set}"
U=$FAKE_PI/units
echo "$what $*" >> "$FAKE_PI/log"
case $what in
    systemctl)
        sub=${1:-}; shift
        case $sub in
            is-active)
                [ -f "$U/$1" ] || { echo inactive; exit 3; }
                cat "$U/$1"; [ "$(cat "$U/$1")" = active ] ;;
            show)   # show -p LoadState --value UNIT
                u=${*: -1}
                if [ ! -f "$U/$u" ]; then echo not-found; elif [ -f "$U/$u.rtmask" ]; then echo masked; else echo loaded; fi ;;
            mask|unmask)
                now=0; units=()
                for a in "$@"; do case $a in --now) now=1 ;; --runtime) ;; *) units+=("$a") ;; esac; done
                for u in "${units[@]}"; do
                    [ -f "$U/$u" ] || { echo "Unit $u does not exist" >&2; exit 5; }
                    if [ "$sub" = mask ]; then : > "$U/$u.rtmask"; [ "$now" = 1 ] && echo inactive > "$U/$u"
                    else rm -f "$U/$u.rtmask"; fi
                done ;;
            stop)  for u in "$@"; do [ -f "$U/$u" ] && echo inactive > "$U/$u"; done ;;
            start)
                for u in "$@"; do
                    [ -f "$U/$u" ] || { echo "Unit $u not found" >&2; exit 5; }
                    [ -f "$U/$u.rtmask" ] && { echo "Unit $u is masked" >&2; exit 1; }
                    echo active > "$U/$u"
                done ;;
            *) echo "fake systemctl: $sub $*" >&2; exit 2 ;;
        esac ;;
    kill)
        [ "${1:-}" = -TERM ] || { echo "fake kill: only -TERM is ever sent, got: $*" >&2; exit 2; }
        [ -d "$FAKE_PI/proc/$2" ] && : > "$FAKE_PI/proc/$2/termed" ;;
    sleep)
        # No time passes; the processes that are to end come one sleep nearer to it.
        for p in "$FAKE_PI"/proc/[0-9]*; do
            [ -d "$p" ] || continue
            f=""
            if [ -f "$p/ends_in" ]; then f=$p/ends_in
            elif [ -f "$p/ends_after_term" ] && [ -f "$p/termed" ]; then f=$p/ends_after_term; fi
            [ -n "$f" ] || continue
            n=$(( $(cat "$f") - 1 ))
            if [ "$n" -le 0 ]; then rm -rf "$p"; else echo "$n" > "$f"; fi
        done ;;
    date)
        case " $* " in
            *" -s "*) ;;                      # setting it: logged above, nothing done
            *) date "$@" ;;
        esac ;;
    *) echo "fake-pi.sh: $what?" >&2; exit 2 ;;
esac
