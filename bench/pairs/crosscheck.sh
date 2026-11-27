#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# bench/pairs/crosscheck.sh: one cell of bench/pairs measured a second time by a tool
# that is not ours, performance_test (Apex.AI's, the ROS 2 project's usual one; the
# package ros-jazzy-performance-test, 2.3.0). README.md, "A cross-check against
# performance_test", says what it is for and what it found.
#
#   crosscheck.sh NODE [--rounds N] [--seconds S] [--cpus PUB,SUB] [--out DIR]
#
# NODE is ros2_pair_node; ROS 2 Jazzy and the ros2_pair workspace are sourced, and
# `ros2 run performance_test perf_test` works. The cell: 256 bytes at 1 kHz, one
# publisher process and one subscriber process, Fast DDS as it comes, reliable, keep
# last 16 on both tools. N rounds (5), each performance_test then ros2_pair_node, S
# seconds each (20) after 2 s that are not counted, the two processes pinned to the same
# two CPUs for both tools.
#
# What is compared is the MEAN of publish call to callback, because that is the one
# figure performance_test gives (a mean, a smallest and a largest each second, no
# percentile): its message's time stamp is taken before publish(), as ours is.
#
# On a machine without ROS 2, in the container of bench/ros2/Dockerfile (root in a
# container that is thrown away, to install the package; nothing on the machine):
#
#   docker run --rm -v "$PWD/out/baselines/ros2-$(uname -m)":/work:ro \
#       -v "$PWD/bench/pairs":/p:ro hangar-ros2-bench:jazzy bash -c '
#       apt-get update -qq && apt-get install -y -qq ros-jazzy-performance-test &&
#       source /opt/ros/jazzy/setup.bash && source /work/install/setup.bash &&
#       bash /p/crosscheck.sh /work/install/ros2_pair/lib/ros2_pair/ros2_pair_node'
set -eu
node=${1:?crosscheck.sh NODE [--rounds N] [--seconds S] [--cpus PUB,SUB] [--out DIR]}
shift
rounds=5 seconds=20 cpus= out=
while [ $# -gt 0 ]; do
	case $1 in
	--rounds) shift; rounds=$1 ;;
	--seconds) shift; seconds=$1 ;;
	--cpus) shift; cpus=$1 ;;
	--out) shift; out=$1 ;;
	*) echo "crosscheck.sh: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done
[ -n "$out" ] || out=$(mktemp -d)
mkdir -p "$out"
if [ -z "$cpus" ]; then
	# The two highest CPUs this process may use, as the driver takes them.
	cpus=$(python3 -c 'import os; c = sorted(os.sched_getaffinity(0)); print(f"{c[-1]},{c[-2]}")')
fi
pub_cpu=${cpus%,*} sub_cpu=${cpus#*,}
export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST ROS_DOMAIN_ID=77 RMW_IMPLEMENTATION=rmw_fastrtps_cpp
perf=$(ros2 pkg prefix performance_test)/lib/performance_test/perf_test
[ -x "$perf" ] || { echo "crosscheck.sh: no perf_test (the package ros-jazzy-performance-test)" >&2; exit 1; }
total=$((seconds + 2))
for r in $(seq 1 "$rounds"); do
	a="-m Array256 -r 1000 --reliability RELIABLE --history KEEP_LAST --history-depth 16 --max-runtime $total --ignore 2 -t xc$r"
	# shellcheck disable=SC2086
	taskset -c "$sub_cpu" "$perf" $a -p 0 -s 1 -l "$out/perf-$r.json" > "$out/perf-$r-sub.log" 2>&1 &
	sleep 0.5
	# shellcheck disable=SC2086
	taskset -c "$pub_cpu" "$perf" $a -p 1 -s 0 -l "$out/perf-$r-pub.json" > "$out/perf-$r-pub.log" 2>&1 || true
	wait
	taskset -c "$sub_cpu" "$node" --role sub --size 256 --rate 1000 --seconds "$seconds" --warmup 2 --topic "xc$r" \
		> "$out/pair-$r.json" 2> "$out/pair-$r-sub.log" &
	sleep 0.5
	taskset -c "$pub_cpu" "$node" --role pub --size 256 --rate 1000 --seconds "$seconds" --warmup 2 --topic "xc$r" \
		> "$out/pair-$r-pub.json" 2> "$out/pair-$r-pub.log" || true
	wait
done
version=$(dpkg-query -W -f='${Version}' ros-jazzy-performance-test 2> /dev/null || echo "version unknown")
python3 -I - "$out" "$rounds" "$version" <<'E'
import json, statistics, sys
out, rounds = sys.argv[1], int(sys.argv[2])
rows = []
for r in range(1, rounds + 1):
    with open(f'{out}/perf-{r}.json') as f:
        d = json.load(f)
    a = [x for x in d['analysis_results'] if x['latency_n'] > 0]
    n = sum(x['latency_n'] for x in a)
    perf = dict(n=n, mean=sum(x['latency_mean'] * x['latency_n'] for x in a) / n * 1e6, lo=min(x['latency_min'] for x in a) * 1e6,
                hi=max(x['latency_max'] for x in a) * 1e6, lost=sum(x['num_samples_lost'] for x in a))
    with open(f'{out}/pair-{r}.json') as f:
        c = json.loads(f.read().strip().splitlines()[-1])
    h = c['call']
    pair = dict(n=h['n'], mean=h['sum_ns'] / h['n'] / 1e3, lo=h['min_ns'] / 1e3, hi=h['max_ns'] / 1e3, lost=c['lost'])
    rows.append((perf, pair))
    if r == 1:
        print(f'performance_test {sys.argv[3]}, {d["rmw_implementation"]}, {d["msg_name"]}, {d["rate"]} Hz, '
              f'{d["qos_reliability"]}, depth {d["qos_history_depth"]}')
print('publish call to callback, microseconds: mean (smallest .. largest), samples, lost')
for i, (p, q) in enumerate(rows, 1):
    print(f'  round {i}: performance_test {p["mean"]:7.1f} ({p["lo"]:.1f} .. {p["hi"]:.1f}) n={p["n"]} lost={p["lost"]}'
          f'   ros2_pair_node {q["mean"]:7.1f} ({q["lo"]:.1f} .. {q["hi"]:.1f}) n={q["n"]} lost={q["lost"]}')
pm, qm = [p['mean'] for p, _ in rows], [q['mean'] for _, q in rows]
print(f'median of the rounds\' means: performance_test {statistics.median(pm):.1f} (range {min(pm):.1f} .. {max(pm):.1f}), '
      f'ros2_pair_node {statistics.median(qm):.1f} (range {min(qm):.1f} .. {max(qm):.1f}); '
      f'ours / theirs {statistics.median(qm) / statistics.median(pm):.2f}')
print(f'the raw files: {out}')
E
