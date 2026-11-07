#!/usr/bin/env bash
# tools/sim/run.sh: the drivable ArduSub demo. Gazebo Harmonic, ArduSub SITL and the
# ardupilot_gazebo plugin run in one container; QGroundControl runs on the host and
# talks to SITL on 127.0.0.1:14550. The host steps are in SETUP.md.
#
#   run.sh build   build the image from tools/sim/Dockerfile (long: see SETUP.md)
#   run.sh up      start the container and launch Gazebo and SITL inside it
#   run.sh tmux    attach to the tmux session inside (Gazebo log, MAVProxy prompt)
#   run.sh shell   a shell in the running container
#   run.sh down    stop and remove the container (the image stays)
#
# Environment: SIM_CONFIG=heavy|base (BlueROV2 Heavy, frame vectored_6dof, the
# default; or the base BlueROV2, frame vectored), SIM_IMAGE, SIM_NAME;
# HANGAR_SIM=1 puts hangar in the loop: SITL's MAVLink goes to the vehicle's
# mavlink module on 14560 instead of QGC's 14550 (tools/sim/hangar-up.sh).
set -euo pipefail

here=$(command dirname -- "$(command realpath -- "${BASH_SOURCE[0]}")")
image=${SIM_IMAGE:-hangar-sim:harmonic}
name=${SIM_NAME:-hangar-sim}
config=${SIM_CONFIG:-heavy}
work=$here/work

usage() {
    command sed -n '2,15p' "${BASH_SOURCE[0]}" | command sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

exists()  { docker inspect "$name" >/dev/null 2>&1; }
running() { [ "$(docker inspect -f '{{.State.Running}}' "$name" 2>/dev/null || true)" = true ]; }

cmd_build() {
    docker build --tag "$image" --file "$here/Dockerfile" "$here"
    echo "built $image"
}

cmd_up() {
    if running; then
        echo "$name is already running: '$0 tmux' to look at it, '$0 down' to stop it"
        return 0
    fi
    exists && docker rm -f "$name" >/dev/null   # a stopped leftover
    command mkdir -p "$work"

    # Gazebo's GUI is Qt on top of Ogre, and upstream documents that it does not
    # work under Wayland (gazebosim.org/docs/harmonic/troubleshooting: "set
    # QT_QPA_PLATFORM=xcb"). So the GUI uses the host's XWayland display through
    # the X socket; the Wayland socket is passed too, for anything else in the
    # container that wants it, and is not what Gazebo uses.
    : "${DISPLAY:?DISPLAY is not set; the Gazebo GUI needs the XWayland display (SETUP.md)}"
    local -a wayland=()
    if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -S "${XDG_RUNTIME_DIR:-/nonexistent}/$WAYLAND_DISPLAY" ]; then
        wayland=(-e WAYLAND_DISPLAY=wayland-0
                 -v "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:/tmp/wayland-0"
                 -e XDG_RUNTIME_DIR=/tmp)
    fi
    # /dev/input for a joystick later. Devices present now are passed; one plugged
    # in after 'up' is not seen until the next 'up'.
    local -a input=()
    [ -d /dev/input ] && input=(--device /dev/input:/dev/input)

    # --gpus all: the NVIDIA container runtime mounts the host driver's OpenGL and
    # Vulkan libraries (the image sets NVIDIA_DRIVER_CAPABILITIES).
    # --net=host: MAVProxy inside sends to 127.0.0.1:14550, QGroundControl's default
    # UDP port, on the host; SITL and the Gazebo plugin meet on UDP 9002 the same way.
    # The container only sleeps; launch.sh starts the processes, in tmux, so that a
    # crash leaves its output on screen and in work/.
    docker run --detach --name "$name" \
        --gpus all \
        --net=host \
        -e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix \
        -e QT_QPA_PLATFORM=xcb \
        "${wayland[@]}" \
        "${input[@]}" \
        -e SIM_CONFIG="$config" \
        -e HANGAR_SIM="${HANGAR_SIM:-0}" \
        -v "$work:/home/sim/work" \
        -v "$here:/home/sim/hangar-sim:ro" \
        "$image" sleep infinity >/dev/null
    docker exec "$name" /home/sim/hangar-sim/launch.sh start
    cat <<MSG
started $name ($config). Gazebo's window should appear; then start QGroundControl.
  logs:   $work/{gazebo,mavproxy,ardusub}.log
  inside: $0 tmux    (Ctrl-b d detaches; window 'sitl' has the MAVProxy prompt)
  stop:   $0 down
MSG
}

cmd_tmux()  { docker exec -it "$name" tmux attach-session -t sim; }
cmd_shell() { docker exec -it "$name" bash; }

cmd_down() {
    if exists; then
        docker rm -f "$name" >/dev/null
        echo "removed $name"
    else
        echo "$name is not running"
    fi
}

case "${1:-}" in
    build) cmd_build ;;
    up)    cmd_up ;;
    tmux)  cmd_tmux ;;
    shell) cmd_shell ;;
    down)  cmd_down ;;
    -h|--help|help|'') usage 0 ;;
    *) echo "unknown command: $1" >&2; usage 2 ;;
esac
