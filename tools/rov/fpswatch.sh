#!/usr/bin/env bash
# tools/rov/fpswatch.sh: the video's frame rates and the tether's bytes over a window,
# as measured on the lab's BlueROV2 on 2026-11-20 (tools/rov/README.md). lab.sh bench
# runs it on the laptop and on the vehicle's Pi.
#
#   fpswatch.sh [-s SECONDS] [-H HELLION] [-e HANGAR] [-i IFACE]
#
#   -H HELLION  sample `HELLION check` every 2 s: frames per second of each pipeline
#               (hellion counts "N frames in ~1.2 s", which reads ~32 for 30 fps)
#   -e HANGAR   the encoder's own counters from `HANGAR status` (HANGAR_ROOT as set,
#               /dev/shm/hangar-vehicle by default): frames, keyframes over the window
#   -i IFACE    the interface's bytes in and out over the window, as Mbit/s
set -uo pipefail

secs=60 hellion= hangar= iface=
while getopts "s:H:e:i:" o; do
    case $o in
        s) secs=$OPTARG ;;
        H) hellion=$OPTARG ;;
        e) hangar=$OPTARG ;;
        i) iface=$OPTARG ;;
        *) sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
export HANGAR_ROOT=${HANGAR_ROOT:-/dev/shm/hangar-vehicle}

now() { date +%s.%N; }
iface_bytes() { cat "/sys/class/net/$iface/statistics/rx_bytes" "/sys/class/net/$iface/statistics/tx_bytes" 2>/dev/null | tr '\n' ' '; }
encoder_counts() { # "frames keyframes": frames from video.index's sequence (one message
    # a frame, exact), keyframes from the encoder's status line (cut off when its source
    # is a long path: then 0)
    local f k
    f=$("$hangar" topics 2>/dev/null | awk '$1 == "video.index" {
        for (i = 2; i < NF; i++) if ($i == "Hz" || $i == "event") { print $(i+1); exit } }')
    k=$("$hangar" status 2>/dev/null | awk '/^encoder/ {
        for (i = 1; i < NF; i++) if ($(i+1) ~ /^keyframes,/) k = $i; print k + 0 }')
    echo "${f:-0} ${k:-0}"
}

t0=$(now)
[ -n "$iface" ] && read -r rx0 tx0 <<< "$(iface_bytes)"
[ -n "$hangar" ] && read -r f0 k0 <<< "$(encoder_counts)"
samples=$(mktemp)
if [ -n "$hellion" ]; then
    end=$(( $(date +%s) + secs ))
    while [ "$(date +%s)" -lt "$end" ]; do
        timeout 3 "$hellion" check 2>/dev/null |
            grep -oE 'pipeline [a-z0-9-]+: source [^ ]+ \([0-9]+x[0-9]+, [0-9]+ frames in [0-9.]+s' |
            awk '{ p = $2; sub(":", "", p); t = $9; sub("s", "", t); if (t > 0) printf "%s %.1f\n", p, $6 / t }' >> "$samples"
        sleep 2
    done
else
    sleep "$secs"
fi
t1=$(now)

awk '{ s[$1] += $2; n[$1]++; if (!($1 in m) || $2 < m[$1]) m[$1] = $2 }
     END { for (p in s) printf "  hellion %-10s mean %5.1f frames/s, worst %5.1f  (%d samples)\n", p, s[p] / n[p], m[p], n[p] }' "$samples"
rm -f "$samples"
if [ -n "$hangar" ]; then
    read -r f1 k1 <<< "$(encoder_counts)"
    awk -v f0="${f0:-0}" -v f1="${f1:-0}" -v k0="${k0:-0}" -v k1="${k1:-0}" -v t0="$t0" -v t1="$t1" 'BEGIN {
        dt = t1 - t0
        if (f1 <= f0) { print "  encoder  no frames counted (not running?)"; exit }
        printf "  encoder  %.1f frames/s over %.0f s", (f1 - f0) / dt, dt
        if (k1 > k0) printf ", a keyframe every %.1f frames\n", (f1 - f0) / (k1 - k0)
        else printf " (keyframes not readable from its status line)\n" }'
fi
if [ -n "$iface" ]; then
    read -r rx1 tx1 <<< "$(iface_bytes)"
    awk -v a="${rx0:-0}" -v b="${rx1:-0}" -v c="${tx0:-0}" -v d="${tx1:-0}" -v t0="$t0" -v t1="$t1" -v i="$iface" 'BEGIN {
        dt = t1 - t0; printf "  %s over %.0f s: in %.2f Mbit/s, out %.2f Mbit/s\n", i, dt, (b - a) * 8 / dt / 1e6, (d - c) * 8 / dt / 1e6 }'
fi
