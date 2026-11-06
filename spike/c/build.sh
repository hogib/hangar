#!/usr/bin/env bash
# Builds every binary, from clean, for both targets and both optimization levels:
#   out/<target>/<bin>      -std=c11 -Os (the release build)
#   out/<target>/O2/<bin>   -std=c11 -O2 (for size comparison)
# static musl via zig cc, stripped. Writes sizes, line counts and the x86_64
# release build time to out/report.json.
#
# The object cache is out/.zig-cache, removed with out/, so every run compiles
# hangar's own sources from scratch. musl itself stays in zig's global cache,
# prebuilt, as Rust's std is (a cold global cache adds ~11 s, once).
set -euo pipefail
cd "$(dirname "$0")"

TARGETS=(x86_64-linux-musl aarch64-linux-musl)
BINS=(spike-manager talker listener bench)
LIB=src/hg_topic.c
CFLAGS=(-std=c11 -Wall -Wextra -pedantic -Werror -static)

declare -A SRC=(
	[spike-manager]=src/manager.c
	[talker]=src/talker.c
	[listener]=src/listener.c
	[bench]=src/bench.c
)

rm -rf out
mkdir -p out
export ZIG_LOCAL_CACHE_DIR="$PWD/out/.zig-cache"

# build <target> <opt> <dir>: all binaries in parallel.
build() {
	local target=$1 opt=$2 dir=$3 pids=() b
	mkdir -p "$dir"
	for b in "${BINS[@]}"; do
		zig cc -target "$target" "-$opt" "${CFLAGS[@]}" -o "$dir/$b" "${SRC[$b]}" "$LIB" &
		pids+=($!)
	done
	for p in "${pids[@]}"; do wait "$p"; done
	for b in "${BINS[@]}"; do llvm-strip --strip-all "$dir/$b"; done
}

t0=$EPOCHREALTIME
build x86_64-linux-musl Os out/x86_64-linux-musl
t1=$EPOCHREALTIME
build_time=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f", b - a }')

build x86_64-linux-musl O2 out/x86_64-linux-musl/O2
build aarch64-linux-musl Os out/aarch64-linux-musl
build aarch64-linux-musl O2 out/aarch64-linux-musl/O2

# Non-blank, non-comment lines of C (block and line comments removed).
loc() {
	cat "$@" | python3 -c '
import re, sys
s = sys.stdin.read()
s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
s = re.sub(r"//[^\n]*", "", s)
print(sum(1 for l in s.splitlines() if l.strip()))'
}

{
	printf '{\n  "language": "c11",\n  "compiler": "zig cc %s",\n' "$(zig version)"
	printf '  "build_time_s": %s,\n' "$build_time"
	printf '  "loc": {"library": %s, "manager": %s, "programs": %s},\n' \
		"$(loc src/hg_topic.c src/hg_topic.h src/hg_sys.h)" \
		"$(loc src/manager.c)" \
		"$(loc src/talker.c src/listener.c src/bench.c)"
	printf '  "sizes": {\n'
	first=1
	for b in "${BINS[@]}"; do
		[ $first = 1 ] || printf ',\n'
		first=0
		printf '    "%s": {' "$b"
		sep=""
		for t in "${TARGETS[@]}"; do
			printf '%s"%s": {"Os": %d, "O2": %d}' "$sep" "$t" \
				"$(stat -c %s "out/$t/$b")" "$(stat -c %s "out/$t/O2/$b")"
			sep=", "
		done
		printf '}'
	done
	printf '\n  }\n}\n'
} > out/report.json

rm -rf out/.zig-cache
echo "built in ${build_time} s (x86_64, -Os); out/report.json:"
cat out/report.json
