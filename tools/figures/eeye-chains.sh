#!/bin/bash
# tools/figures/eeye-chains.sh [OUT_DIR]: one picture per effect chain, from a running hellion.
#
# For each chain below: eeye's configuration is posted to hellion's page (the same request
# the page's own controls make), the picture is left to settle, and hellion saves a
# snapshot of the pilot's picture (JPEG, quality 92, the stream's own size: nothing is
# scaled or photographed off a screen). `camera` is the untouched frame. The snapshots
# are copied to OUT_DIR under the chain's name, with chains.txt saying what each was.
#
# Needs hellion and eeye running with a source: tools/rov/lab.sh up --video, or the
# rehearsal of tools/rov/QUICKSTART.md with a file (LAB_HOST=local H264_NODE=file:...).
# HELLION (http://127.0.0.1:8092) is the page; SETTLE (6) the seconds waited after a
# change: auto visibility moves toward its correction with a time constant.
set -euo pipefail

out=${1:-figures/eeye}
HELLION=${HELLION:-http://127.0.0.1:8092}
SETTLE=${SETTLE:-6}
state=${LAB_STATE_DIR:-$HOME/.cache/hangar-lab}

mkdir -p "$out"
before=$(curl -fsS -m 5 "$HELLION/config") || { echo "no hellion at $HELLION" >&2; exit 1; }
restore() { curl -fsS -m 5 -X POST -H 'Content-Type: application/json' --data-binary "$before" "$HELLION/config" > /dev/null || true; }
trap restore EXIT

snap() { # name src
    local file age
    # A source that has stopped leaves the last picture standing, and a snapshot of it
    # would be the chain before: the pilot's picture must be under a second old.
    age=$(curl -fsS -m 5 "$HELLION/health" | sed -n 's/.*"link":{[^}]*"age_s":\([0-9.]*\).*/\1/p')
    awk -v a="${age:-99}" 'BEGIN { exit !(a < 1.0) }' ||
        { echo "$1: the pilot's picture is ${age:-of unknown age} s old: the source has stopped (a file source plays once)" >&2; return 1; }
    file=$(curl -fsS -m 15 -X POST "$HELLION/snapshot?src=$2" | sed -n 's/.*"file":"\([^"]*\)".*/\1/p')
    [ -n "$file" ] || { echo "$1: no snapshot" >&2; return 1; }
    local path
    path=$(find "$state" /var/lib/hellion/snapshots -name "$file" 2>/dev/null | head -1)
    [ -n "$path" ] || { echo "$1: $file was saved, but not where this script looks" >&2; return 1; }
    cp "$path" "$out/$1.jpg"
    echo "$1.jpg  $(file -b "$out/$1.jpg" | sed -n 's/.*, \([0-9]*x[0-9]*\),.*/\1/p')"
}

chain() { # name auto_visibility chain-json description
    local cfg
    cfg=$(printf '{"capture_source":"bus","capture_stream":"camera","output_device":"","auto_visibility":%s,"chain":%s,"record_path":""}' "$2" "$3")
    curl -fsS -m 5 -X POST -H 'Content-Type: application/json' --data-binary "$cfg" "$HELLION/config" > /dev/null ||
        { echo "$1: the configuration was refused" >&2; return 1; }
    sleep "$SETTLE"
    snap "$1" pilot
    printf '%s\tauto_visibility %s\tchain %s\t%s\n' "$1" "$2" "$3" "$4" >> "$out/chains.txt"
}

: > "$out/chains.txt"
snap camera camera
printf 'camera\t-\t-\tthe camera frame as it arrived, before eeye\n' >> "$out/chains.txt"
chain none            false '[]'                                        'an empty chain: the pilot picture is the camera picture'
chain auto            true  '[]'                                        'auto visibility alone: colour cast, brightness and contrast, continuously'
chain contrast        false '[{"effect":"contrast"}]'                   'the min/max luma stretch, one frame at a time'
chain sobel           false '[{"effect":"sobel"}]'                      'gradient magnitude'
chain blur-sobel      false '[{"effect":"blur"},{"effect":"sobel"}]'    'the example chain: a Gaussian, then the gradient'
chain log             false '[{"effect":"log"}]'                        'Laplacian of Gaussian, zero crossings'
chain canny           false '[{"effect":"canny"}]'                      'Canny, default thresholds'
chain auto-canny      true  '[{"effect":"canny"}]'                      'auto visibility, then Canny on the corrected picture'
echo "in $out; the configuration before is put back"
