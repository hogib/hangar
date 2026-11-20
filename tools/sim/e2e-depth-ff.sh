#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# docs/09's adaptive depth hold, end to end in simulation on one machine:
# sim_vehicle standalone, nav, depth_hold, the mux, safety, the thrusters, the
# pairer, the recorder and policy_depth_ff under a manager (profiles/sim-depth-ff),
# then tools/sim/e2e_depth_ff.py: arm, assisted, train live in PyTorch, deploy,
# residual:depth_ff, buoyancy changes, and the measurements.
#
#   tools/sim/e2e-depth-ff.sh [up|run|down|status]     (default: run = up, the experiment, down)
#
# Environment: E2E_ROOT (/dev/shm/hangar-e2e; the manager's root, a short path:
# the control socket's must fit sun_path), E2E_PYTHON (a python with torch and
# numpy; default ~/.cache/hangar-e2e-venv/bin/python), E2E_OUT (where
# events.log, report.json, report.md and a copy of the recorder's session go;
# default $E2E_ROOT/out; E2E_KEEP_SESSION=0 leaves the session out), E2E_ARGS
# (passed on to e2e_depth_ff.py, e.g. "--quick" or "--label shortfall"),
# E2E_WHERE=armed (the dataset's Where without the stillness terms),
# E2E_BOUND=0.15 (depth_ff.py's bounded model, and the policy's Arch with it).
# The method and a run's results: bench/e2e-depth-ff/README.md.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
builddir=$repo/builddir
prof=$repo/profiles/sim-depth-ff
root=${E2E_ROOT:-/dev/shm/hangar-e2e}
py=${E2E_PYTHON:-$HOME/.cache/hangar-e2e-venv/bin/python}
out=${E2E_OUT:-$root/out}

need() {
    for b in hangard hangar hangar-sim-vehicle hangar-nav hangar-depth-hold hangar-mux hangar-safety \
             hangar-thrusters hangar-policy pairer recorder; do
        [ -x "$builddir/$b" ] || { echo "builddir/$b missing: make -j8 all" >&2; exit 2; }
    done
    "$py" -c 'import torch, numpy' 2>/dev/null || {
        echo "$py has no torch: python3 -m venv ~/.cache/hangar-e2e-venv && ~/.cache/hangar-e2e-venv/bin/pip install --index-url https://download.pytorch.org/whl/cpu torch numpy" >&2
        exit 2
    }
}

# The model's architecture hash, from an export of the example's model: the
# policy spec must name the Arch the trainer exports.
arch() {
    local tmp=$root/arch.hgw
    (cd "$repo/python/examples" && "$py" depth_ff.py --synthetic --steps 1 --out "$tmp" \
        ${E2E_BOUND:+--bound "$E2E_BOUND"} > /dev/null)
    PYTHONPATH=$repo/python "$py" -c "
from hangar.nn import hgw; import sys
print('%016x' % hgw.arch_hash(hgw.read(sys.argv[1]).graph))" "$tmp"
    command rm -f "$tmp"
}

lay_out() {
    command mkdir -p "$root/etc/hangar/modules" "$root/etc/hangar/datasets" "$root/etc/hangar/policies" \
        "$root/usr/share/hangar/msg" "$root/var/lib/hangar/safety" "$root/var/lib/hangar/depth_hold" \
        "$root/var/lib/hangar/policy_depth_ff/incoming" \
        "$root/var/lib/hangar/recorder" "$root/run/hangar" "$out"
    command cp "$repo"/msg/*.msg "$root/usr/share/hangar/msg/"
    local f
    for f in "$prof"/*.module; do
        command sed -e "s|/usr/lib/hangar/|$builddir/|g" -e "s|/etc/hangar/|$root/etc/hangar/|g" "$f" \
            > "$root/etc/hangar/modules/$(basename "$f")"
    done
    command cp "$prof/hangard.conf" "$root/etc/hangar/hangard.conf"
    command cp "$prof/safety.params" "$root/var/lib/hangar/safety/params"
    command cp "$prof/depth_hold.params" "$root/var/lib/hangar/depth_hold/params"
    for f in "$prof"/*.conf; do
        [ "$(basename "$f")" = hangard.conf ] && continue
        command sed -e "s|/etc/hangar/|$root/etc/hangar/|g" -e "s|/dev/shm/hangar-vehicle|$root|g" "$f" \
            > "$root/etc/hangar/$(basename "$f")"
    done
    command cp "$prof"/datasets/*.dataset "$root/etc/hangar/datasets/"
    # E2E_WHERE=armed: the variant with loss on every row the outputs were
    # enabled in, the vehicle moving or not (docs/09's spec keeps the still rows).
    if [ "${E2E_WHERE:-still}" = armed ]; then
        command sed -i -e 's|^Where=.*|Where=safety.state.outputs_enabled == 1|' "$root/etc/hangar/datasets/depth_ff.dataset"
        echo "dataset depth_ff: $(grep '^Where=' "$root/etc/hangar/datasets/depth_ff.dataset")"
    fi
    local a
    a=$(arch)
    command sed -e "s|^Arch=.*|Arch=$a|" "$prof/policies/depth_ff.policy" > "$root/etc/hangar/policies/depth_ff.policy"
    echo "policy_depth_ff: Arch=$a"
}

up() {
    need
    # The control socket's path must fit sun_path (108 bytes).
    if [ ${#root} -gt 80 ]; then
        echo "E2E_ROOT is ${#root} characters, too long for the control socket's path: $root" >&2
        exit 2
    fi
    if [ -f "$root/hangard.pid" ] && kill -0 "$(command cat "$root/hangard.pid")" 2>/dev/null; then
        echo "a manager already runs under $root"; return 0
    fi
    command rm -rf "$root/etc" "$root/var" "$root/run" "$root/dev"
    lay_out
    HANGAR_ROOT=$root nohup "$builddir/hangard" >> "$root/hangard.log" 2>&1 &
    echo $! > "$root/hangard.pid"
    local i ready=0
    for i in $(seq 1 100); do
        if HANGAR_ROOT=$root "$builddir/hangar" status 2>/dev/null | grep -q '^policy_depth_ff *ready'; then
            ready=1
            break
        fi
        sleep 0.1
    done
    if [ $ready = 0 ]; then
        echo "the manager under $root did not bring policy_depth_ff to ready:" >&2
        HANGAR_ROOT=$root "$builddir/hangar" status >&2 || command tail -n 5 "$root/hangard.log" >&2
        down
        exit 1
    fi
    HANGAR_ROOT=$root "$builddir/hangar" status
}

down() {
    if [ -f "$root/hangard.pid" ]; then
        local pid
        pid=$(command cat "$root/hangard.pid")
        kill -TERM "$pid" 2>/dev/null || true
        local i
        for i in $(seq 1 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        command rm -f "$root/hangard.pid"
    fi
}

case "${1:-run}" in
    up) up ;;
    down) down ;;
    status) HANGAR_ROOT=$root "$builddir/hangar" status ;;
    run)
        up
        trap down EXIT
        HANGAR_ROOT=$root HANGAR_BIN=$builddir/hangar PYTHONPATH=$repo/python \
            "$py" "$repo/tools/sim/e2e_depth_ff.py" --root "$root" --out "$out" \
            ${E2E_BOUND:+--bound "$E2E_BOUND"} ${E2E_ARGS:-}
        # The recording outlives the root: e2e_depth_ff.py --analyse reads it again.
        down
        trap - EXIT
        if [ "${E2E_KEEP_SESSION:-1}" = 1 ]; then
            command rm -rf "$out/session"
            command cp -r "$(command ls -d "$root"/var/lib/hangar/recorder/sessions/*/ | command tail -n 1)" "$out/session"
            echo "the session: $out/session ($(du -sh "$out/session" | cut -f1))"
        fi
        ;;
    *) echo "usage: $0 [up|run|down|status]" >&2; exit 2 ;;
esac
