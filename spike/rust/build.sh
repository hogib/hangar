#!/bin/sh
# Builds every binary, static musl, stripped, for x86_64 and aarch64, at opt-level "s"
# (out/<target>/) and at opt-level 3 (out/<target>/o3/), from clean, and writes
# out/report.json: sizes, lines of code, unsafe counts, clean build time.
set -eu
cd "$(dirname "$0")"

TARGETS="x86_64-unknown-linux-musl aarch64-unknown-linux-musl"
BINS="spike-manager talker listener bench"

cargo clean -q
rm -rf out
start=$(date +%s%N)
cargo build -q --release --target x86_64-unknown-linux-musl
end=$(date +%s%N)
build_time=$(awk "BEGIN { printf \"%.2f\", ($end - $start) / 1e9 }")
echo "clean release build, x86_64: ${build_time} s"

for t in $TARGETS; do
    for profile in release release-o3; do
        cargo build -q --profile "$profile" --target "$t"
        if [ "$profile" = release ]; then dest="out/$t"; else dest="out/$t/o3"; fi
        mkdir -p "$dest"
        for b in $BINS; do
            cp "target/$t/$profile/$b" "$dest/$b"
        done
    done
done

# Non-blank, non-comment lines of Rust (no block comments are used).
loc() {
    cat "$@" | grep -v '^[[:space:]]*$' | grep -cv '^[[:space:]]*//'
}

# unsafe blocks, and the non-blank, non-comment lines they span.
unsafe_counts() {
    awk '
        function count_braces(s,   i, c) {
            for (i = 1; i <= length(s); i++) {
                c = substr(s, i, 1)
                if (c == "{") depth++
                else if (c == "}") depth--
            }
        }
        {
            code = $0
            sub(/\/\/.*/, "", code)
            if (!inside) {
                p = index(code, "unsafe {")
                if (p == 0) next
                blocks++
                inside = 1
                depth = 0
                code = substr(code, p)
            }
            if (code ~ /[^[:space:]]/) lines++
            count_braces(code)
            if (depth <= 0) inside = 0
        }
        END { printf "%d %d\n", blocks, lines }
    ' "$@"
}

lib_loc=$(loc hangar/src/*.rs)
mgr_loc=$(loc manager/src/*.rs)
prog_loc=$(loc programs/src/bin/*.rs)
set -- $(unsafe_counts hangar/src/*.rs manager/src/*.rs programs/src/bin/*.rs)
unsafe_blocks=$1
unsafe_lines=$2

size() { stat -c %s "$1"; }
{
    echo "{"
    echo "  \"sizes\": {"
    first_b=1
    for b in $BINS; do
        [ $first_b = 1 ] || echo ","
        first_b=0
        printf '    "%s": {' "$b"
        first_t=1
        for t in $TARGETS; do
            [ $first_t = 1 ] || printf ', '
            first_t=0
            printf '"%s": {"s": %s, "3": %s}' "$t" "$(size out/$t/$b)" "$(size out/$t/o3/$b)"
        done
        printf '}'
    done
    echo ""
    echo "  },"
    echo "  \"loc\": {\"library\": $lib_loc, \"manager\": $mgr_loc, \"programs\": $prog_loc},"
    echo "  \"unsafe_blocks\": $unsafe_blocks,"
    echo "  \"unsafe_lines\": $unsafe_lines,"
    echo "  \"build_time_s\": $build_time"
    echo "}"
} > out/report.json
cat out/report.json
