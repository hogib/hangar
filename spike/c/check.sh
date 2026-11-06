#!/usr/bin/env bash
# Short correctness checks on x86_64 (not timing): bench torn for 3 s,
# bench lost_wake with 20 000 messages, and the manager restarting a talker
# that crashes after 500 messages while a listener runs.
set -uo pipefail
cd "$(dirname "$0")"
B=$PWD/out/x86_64-linux-musl
[ -x "$B/bench" ] || { echo "run ./build.sh first" >&2; exit 1; }

D=$(mktemp -d /dev/shm/hangar-spike-c-check.XXXXXX)
M=
cleanup() {
	[ -n "$M" ] && kill -KILL "$M" 2>/dev/null
	rm -rf "$D"
}
trap cleanup EXIT
export HANGAR_TOPIC_DIR=$D/topics
mkdir -p "$HANGAR_TOPIC_DIR"

echo "== bench torn, 3 s"
timeout 60 "$B/bench" torn --torn-seconds 3 --runs 1
echo "== bench lost_wake, 20000 messages"
timeout 120 "$B/bench" lost_wake --lost-msgs 20000 --runs 1

echo "== manager: talker --crash-after 500 at 1 kHz, listener, 4.5 s"
cat > "$D/config" <<EOF
talker spike.chat $B/talker spike.chat 1000 --crash-after 500
listener - $B/listener spike.chat 60
EOF
"$B/spike-manager" "$D/config" > "$D/stdout" 2> "$D/stderr" &
M=$!
sleep 1.2
echo "-- registry at 1.2 s"
cat "$D/registry"
sleep 3.3
kill -TERM "$M"
wait "$M"
echo "-- manager exit status $?"
M=
echo "-- manager log"
cat "$D/stderr"
echo "-- listener"
cat "$D/stdout"
echo "-- registry removed: $([ -e "$D/registry" ] && echo no || echo yes)"
