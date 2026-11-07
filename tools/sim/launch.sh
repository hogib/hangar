#!/usr/bin/env bash
# tools/sim/launch.sh: runs INSIDE the container (run.sh bind-mounts tools/sim at
# /home/sim/hangar-sim, read-only, so this file can be edited without a rebuild).
#
#   launch.sh start        Gazebo and ArduSub SITL in tmux session "sim"
#   launch.sh stop         kill that session
#   launch.sh ritw FILE    internal: ArduPilot's run_in_terminal_window.sh calls
#                          "$SITL_RITW_TERMINAL FILE" to start the ArduSub binary;
#                          this runs FILE with its output in work/ardusub.log
#
# SIM_CONFIG (set by run.sh): heavy -> BlueROV2 Heavy, frame vectored_6dof,
# world bluerov2_heavy_underwater.world; base -> BlueROV2, frame vectored,
# world bluerov2_underwater.world (bluerov2_gz README, "Running Gazebo").
set -euo pipefail

work=/home/sim/work
self=$(realpath -- "${BASH_SOURCE[0]}")

case "${SIM_CONFIG:-heavy}" in
    heavy) frame=vectored_6dof; world=bluerov2_heavy_underwater.world ;;
    base)  frame=vectored;      world=bluerov2_underwater.world ;;
    *) echo "SIM_CONFIG must be heavy or base, not '$SIM_CONFIG'" >&2; exit 2 ;;
esac

start() {
    mkdir -p "$work"
    if tmux has-session -t sim 2>/dev/null; then
        echo "session 'sim' already running"
        return 0
    fi
    # Gazebo first: the model's ArduPilotPlugin listens on UDP 9002 and SITL, started
    # with --model JSON, sends to 127.0.0.1:9002 and waits until something answers.
    # -r runs the world on start, -v 4 is verbose. remain-on-exit keeps a crashed
    # window's output on screen.
    tmux new-session -d -s sim -n gazebo -c /home/sim \
        "gz sim -v 4 -r $world 2>&1 | tee -a $work/gazebo.log"
    tmux set-option -t sim remain-on-exit on

    # ArduSub SITL with MAVProxy in front. Flags, from sim_vehicle.py at the pinned tag:
    #   -v ArduSub -f $frame   vehicle and frame (Tools/autotest/pysim/vehicleinfo.py:
    #                          vectored -> sub.parm, vectored_6dof -> sub-6dof.parm)
    #   --model JSON           physics from the JSON backend (Gazebo) instead of SITL's own
    #   -L RATBeach            home location (Tools/autotest/locations.txt), as in the
    #                          bluerov2_gz README
    #   -N                     --no-rebuild: ardusub was built in the image; the runtime
    #                          environment differs from the build's, so waf would rebuild
    #   -w                     --wipe-eeprom: default parameters every start, which the
    #                          bluerov2_gz README asks for when switching frames
    # No --out is passed: sim_vehicle.py itself gives MAVProxy --out 127.0.0.1:14550
    # (start_mavproxy(), ports = [14550 + 10 * instance]); QGroundControl listens there.
    # SITL_RITW_TERMINAL makes run_in_terminal_window.sh start the ArduSub binary
    # through "launch.sh ritw", with its output in work/ardusub.log.
    # HANGAR_SIM=1 (set by run.sh from the host's environment): hangar is in the
    # loop. MAVProxy then sends SITL's MAVLink to the vehicle's mavlink module on
    # 127.0.0.1:14560 (modules/PHASE1.md "Ports") and not to 14550, where the
    # topside's module now serves QGroundControl as the vehicle. --no-extra-ports
    # is sim_vehicle.py's switch for the default 14550/14551 outputs (checked at
    # Sub-4.7.2: Tools/autotest/sim_vehicle.py, "Disable setup of UDP 14550 and
    # 14551 output"). Without HANGAR_SIM the demo is as before: QGC on SITL directly.
    local -a hangar_out=()
    if [ "${HANGAR_SIM:-0}" = 1 ]; then
        hangar_out=(--no-extra-ports --out 127.0.0.1:14560)
    fi
    tmux new-window -t sim -n sitl -c /home/sim/ardupilot \
        "SITL_RITW_TERMINAL='$self ritw' sim_vehicle.py -v ArduSub -f $frame --model JSON -L RATBeach -N -w ${hangar_out[*]}"
    tmux pipe-pane -t sim:sitl -o "cat >> $work/mavproxy.log"
    echo "started: Gazebo ($world) and ArduSub SITL (-f $frame${HANGAR_SIM:+, MAVLink to hangar on 14560}); logs in $work"
}

stop() {
    if tmux has-session -t sim 2>/dev/null; then
        tmux kill-session -t sim
        echo "stopped"
    else
        echo "not running"
    fi
}

ritw() {
    # SITL exits if its parent dies (libraries/AP_HAL_SITL/SITL_State.cpp:
    # "make sure we die if our parent dies", kill(_parent_pid, 0)), so this process
    # stays alive as the parent: run the file in the foreground, no exec.
    mkdir -p "$work"
    "$1" >>"$work/ardusub.log" 2>&1
}

case "${1:-}" in
    start) start ;;
    stop)  stop ;;
    ritw)  ritw "${2:?ritw needs the file to run}" ;;
    *) echo "usage: launch.sh start|stop|ritw FILE" >&2; exit 2 ;;
esac
