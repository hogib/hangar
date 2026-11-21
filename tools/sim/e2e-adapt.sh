#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The adaptation experiment of docs/09's adaptive depth hold, whole, in one
# command (bench/e2e-depth-ff/README.md, "Adaptation", has the design and the
# criterion): for each seed one run of tools/sim/e2e-depth-ff2.sh with
# "--adapt" (learned) and one with "--adapt --variant oracle", then the table
# over all of them with the criterion's verdict.
#
#   tools/sim/e2e-adapt.sh [out]     (default: bench/e2e-depth-ff/<today>-adapt[-$E2E_LABEL])
#
# 18 minutes a run, in real time; with five seeds and two runs at a time, an
# hour and a half. The runs are timing-sensitive: make them on a quiet machine.
# Each run's report holds the load average it was made under and its control
# loop's delay, and the table lists them.
#
# Environment: E2E_SEEDS ("1 2 3 4 5": the simulator's and the trainer's),
# E2E_JOBS (2: runs at a time, a learned and an oracle run of one seed
# together), E2E_PIN (1: each run on its own share of the machine's threads,
# with taskset; 0: not pinned), E2E_ROOT_BASE (/dev/shm/hg-adapt: the runs'
# roots are $E2E_ROOT_BASE-0, -1; short, the control socket's path must fit
# sun_path), E2E_LABEL (a suffix of the default out), E2E_EXTRA (passed on to
# e2e_depth_ff2.py: "--quick" for a smoke run, whose numbers mean nothing),
# E2E_TIMEOUT (2400: seconds after which a run is given up), E2E_PYTHON.
set -uo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
py=${E2E_PYTHON:-$HOME/.cache/hangar-e2e-venv/bin/python}
seeds=${E2E_SEEDS:-1 2 3 4 5}
jobs=${E2E_JOBS:-2}
base=${E2E_ROOT_BASE:-/dev/shm/hg-adapt}
out=${1:-$repo/bench/e2e-depth-ff/$(date +%F)-adapt${E2E_LABEL:+-$E2E_LABEL}}

if [ -e "$out/summary.md" ]; then
    echo "$out holds a finished set: name another directory (or E2E_LABEL)" >&2
    exit 2
fi
command mkdir -p "$out"
out=$(cd "$out" && pwd)

threads=$(nproc)
# The threads of slot $1: the machine's threads in $jobs equal shares.
cpus() {
    local per=$((threads / jobs))
    echo "$(($1 * per))-$(($1 * per + per - 1))"
}

machine() {
    {
        echo "$1: $(date -Is); uptime:$(uptime)"
    } >> "$out/machine.txt"
}

{
    echo "tools/sim/e2e-adapt.sh: seeds $seeds; $jobs runs at a time; pinned: ${E2E_PIN:-1}; extra: ${E2E_EXTRA:-none}"
    echo "tree: $(git -C "$repo" rev-parse --short HEAD)$(git -C "$repo" diff --quiet HEAD -- tools python modules lib msg profiles 2>/dev/null || echo ' with changes not committed')"
    echo "machine: $(uname -srm); $threads threads; $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
    echo "governor: $(command cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
} > "$out/machine.txt"

# One run: slot, variant, seed.
one() {
    local slot=$1 variant=$2 seed=$3 dir="$out/$2-s$3" root="$base-$1" pin=()
    command mkdir -p "$dir"
    if [ "${E2E_PIN:-1}" = 1 ] && [ "$jobs" -gt 1 ] && command -v taskset > /dev/null; then
        pin=(taskset -c "$(cpus "$slot")")
    fi
    local args="--adapt ${E2E_EXTRA:-}"
    [ "$variant" = oracle ] && args="$args --variant oracle"
    if E2E_ROOT=$root E2E_OUT=$dir E2E_SEED=$seed E2E_ARGS=$args E2E_PYTHON=$py \
        timeout "${E2E_TIMEOUT:-2400}" "${pin[@]}" "$repo/tools/sim/e2e-depth-ff2.sh" run > "$dir/run.log" 2>&1; then
        command rm -f "$dir/run.log" "$dir/events.json"
        echo "done: $variant, seed $seed"
    else
        echo "FAILED: $variant, seed $seed: $dir/run.log" | command tee -a "$out/machine.txt"
        E2E_ROOT=$root "$repo/tools/sim/e2e-depth-ff2.sh" down > /dev/null 2>&1
    fi
    command rm -rf "$root"
}

# A learned and an oracle run of one seed side by side: the two variants meet
# the same machine.
queue=()
for s in $seeds; do
    queue+=("learned $s" "oracle $s")
done
k=0
while [ $k -lt ${#queue[@]} ]; do
    machine "before ${queue[*]:$k:$jobs}"
    slot=0
    for q in "${queue[@]:$k:$jobs}"; do
        # shellcheck disable=SC2086
        one $slot $q &
        slot=$((slot + 1))
        sleep 2
    done
    wait
    k=$((k + jobs))
done
machine "after the last run"

dirs=()
for d in "$out"/*/; do
    [ -f "$d/report.json" ] && dirs+=("$d")
done
if [ ${#dirs[@]} = 0 ]; then
    echo "no run left a report" >&2
    exit 1
fi
{
    echo "# $(basename "$out")"
    echo
    echo '```'
    command cat "$out/machine.txt"
    echo '```'
    echo
    PYTHONPATH=$repo/python "$py" "$repo/tools/sim/e2e_depth_ff2.py" --summarise "${dirs[@]}"
} > "$out/summary.md"
command cat "$out/summary.md"
