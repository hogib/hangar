#!/bin/bash
# bench/pairs/ros2_pair/repro-datasharing.sh: the smallest case found of Fast DDS's
# data-sharing writer running out of samples for good (bench/pairs/README.md, "Fast DDS,
# data sharing and a reliable writer"). Kept so that the finding can be checked, and
# checked again against a later Fast DDS.
#
#   . /opt/ros/jazzy/setup.bash; . <workspace>/install/setup.bash
#   repro-datasharing.sh [RUNS] [-- node options]     default: 8 runs, "--loan --qos queue"
#
# One writer (ros2_graph_node, one publication of 128 bytes at 50 Hz) and seven readers
# that are started one after another, 0.15 s apart, while it publishes: what the modules
# of a graph do to one another when a system comes up. A line a run: what the writer
# sent and what it failed to send, and what each reader measured.
#
# Seen with Fast DDS 2.14.7 (ros-jazzy-fastrtps 2.14.7, rmw_fastrtps_cpp 8.4.5), x86-64,
# in the container of bench/ros2/Dockerfile, 2026-11-28:
#   --loan --qos queue    (reliable, loaned)        2 runs of 6 stall
#   --qos queue           (reliable, not loaned)    3 runs of 8 stall
#   --loan --qos latest   (best effort, loaned)     0 runs of 8 stall
# A run that stalls sends 215 or so and then fails every publication to the end; the
# readers that joined last have 17 to 19 messages each: one pool's worth (history depth
# 16 plus extra_samples 1).
set -u
runs=8
case ${1:-} in ''|--) ;; *) runs=$1; shift ;; esac
[ "${1:-}" = -- ] && shift
opts=${*:---loan --qos queue}
node=$(command -v ros2_graph_node || true)
[ -n "$node" ] || node=$(dirname "$(command -v ros2_pair_node 2> /dev/null || echo /nonexistent/x)")/ros2_graph_node
[ -x "$node" ] || { echo "repro-datasharing.sh: no ros2_graph_node: source the workspace's setup first" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST ROS_DOMAIN_ID=79
export RMW_FASTRTPS_USE_QOS_FROM_XML=1 ROS_DISABLE_LOANED_MESSAGES=0
export FASTRTPS_DEFAULT_PROFILES_FILE=${FASTRTPS_DEFAULT_PROFILES_FILE:-$here/config/fastdds-datasharing.xml}
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
stalled=0
for run in $(seq 1 "$runs"); do
	now=$(python3 -c 'import time; print(time.monotonic_ns())')
	from=$((now + 2000000000)); until=$((from + 5000000000)); p=r$$x$run
	# shellcheck disable=SC2086
	"$node" --module w --prefix $p --from $from --until $until --pub s:128:50 -- $opts > "$d/w.out" 2> "$d/w.err" &
	w=$!
	for i in 1 2 3 4 5 6 7; do
		sleep 0.15
		# shellcheck disable=SC2086
		"$node" --module r$i --prefix $p --from $from --until $until --sub s:128:50 -- $opts > "$d/r$i.out" 2> /dev/null &
	done
	wait $w
	wait
	sent=$(grep -o '"sent": [0-9]*, "send_failed": [0-9]*' "$d/w.out")
	got=$(for i in 1 2 3 4 5 6 7; do grep -o '"measured": [0-9]*' "$d/r$i.out" | tr -dc '0-9\n'; done | tr '\n' ' ')
	case $sent in *'"send_failed": 0') ;; *) stalled=$((stalled + 1)) ;; esac
	echo "run $run: writer $sent | readers measured: $got"
done
echo "$stalled of $runs runs stalled ($opts)"
