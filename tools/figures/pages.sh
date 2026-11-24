#!/bin/bash
# tools/figures/pages.sh VIDEO.h264 [OUT_DIR]: the paper's pictures of hellion's page and
# Dawn Patrol, from the whole stack on this machine with nothing of the vehicle's.
#
# The stack is tools/rov/lab.sh's rehearsal (QUICKSTART, "Rehearsing without the
# vehicle"): the x86-64 release in ~/hangar-labtest (lab.sh deploy first), the tests'
# fake ArduSub on a pseudo-terminal in the Pixhawk's place (make check builds it), the
# encoder sending VIDEO (H.264, under 64 MB; it is played once, so it must last the
# three minutes this takes), electric-eye's hellion and eeye. It is armed through
# safety, so the pages show a vehicle in use; then tools/figures/pages.py takes the
# pictures, and everything is taken down again.
#
# PLAYWRIGHT_PYTHON is the Python that has Playwright (pipx's by default).
set -euo pipefail
here=$(dirname -- "$(realpath -- "${BASH_SOURCE[0]}")")
repo=$(realpath "$here/../..")
video=$(realpath "$1"); out=$(realpath -m "${2:-figures/pages}")
py=${PLAYWRIGHT_PYTHON:-$HOME/.local/share/pipx/venvs/playwright/bin/python}
[ -x "$py" ] || { echo "pages: no Playwright Python at $py (pipx install playwright; playwright install chromium)" >&2; exit 1; }
fake=$(ls "$repo"/builddir/tests-*/mavlink-modules/fake_ardusub 2>/dev/null | head -1)
[ -x "$fake" ] || { echo "pages: no fake ArduSub: make check builds builddir/tests-*/mavlink-modules/fake_ardusub" >&2; exit 1; }
[ -x "$HOME/hangar-labtest/bin/hangard" ] || { echo "pages: LAB_HOST=local tools/rov/lab.sh deploy <the x86_64 release> first" >&2; exit 1; }

work=$(mktemp -d)
export LAB_HOST=local H264_NODE=file:$video FCU=$work/pixhawk:115200
lab=$repo/tools/rov/lab.sh
hangar() { HANGAR_ROOT=/dev/shm/hangar-vehicle "$HOME/hangar-labtest/bin/hangar" "$@"; }
finish() {
    hangar call safety.disarm > /dev/null 2>&1 || true
    "$lab" down 2>&1 | tail -1
    [ -z "${fpid:-}" ] || kill "$fpid" 2>/dev/null || true
    rm -rf "$work"
}
trap finish EXIT

timeout 900 "$fake" --pty "$work/pixhawk" --control 127.0.0.1:14999 --gcs-sysid 255 --depth 2.4 \
    --params "$HOME/hangar-labtest/share/hangar/profiles/rov-vehicle/ardusub.params" > "$work/fake.log" 2>&1 &
fpid=$!
for _ in $(seq 1 50); do [ -e "$work/pixhawk" ] && break; sleep 0.1; done
[ -e "$work/pixhawk" ] || { echo "pages: the fake ArduSub did not start"; cat "$work/fake.log"; exit 1; }

"$lab" up --serial --video --no-battery-check 2>&1 | grep -E "READY|pipeline camera-in: source|stopped here" || true
# Dawn Patrol's Fly view takes its picture from an MJPEG stream it is told of, and the
# Hellion tab frames hellion's page: both are this laptop's hellion here, so the laid-out
# manifest (not the profile's) is given them and the console started again.
mod=/dev/shm/hangar-vehicle/etc/hangar/modules/console.module
sed -i 's|--hellion http://127.0.0.1:8091/|--hellion http://127.0.0.1:8092/ --preview http://127.0.0.1:8092/stream?src=pilot|' "$mod"
grep -q -- '--preview' "$mod" || echo "pages: the console's manifest has no --hellion to replace: the Fly view will have no picture"
hangar reload > /dev/null && hangar restart console > /dev/null && sleep 2
hangar call safety.arm > /dev/null 2>&1 || echo "pages: not armed: $(hangar call safety.check 2>&1 | grep detail | cut -c1-160)"
sleep 2
hangar status | cut -c1-150
"$py" -I "$here/pages.py" "$out"
