#!/bin/bash
# Builds main.zl twice (default: error return traces on; and
# --no-error-trace) and main.go, runs the three RUNS times alternating
# (checksums must be equal), prints the best microseconds of every row and
# the ratios to Go.
# Usage: benchmarks/defer_errors_iface/bench.sh   (also run by scripts/bench-perf.sh)
# Environment: ZELL=<compiler>, RUNS=N (default 1), SCALE=small.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ZELL=${ZELL:-$root/target/release/zell}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"$ZELL" build "$here/main.zl" --release -o "$out/zell" >/dev/null
"$ZELL" build "$here/main.zl" --release --no-error-trace -o "$out/notrace" >/dev/null
(cd "$here" && go build -o "$out/go" main.go)
python3 "$root/scripts/bench-rows.py" "${RUNS:-1}" zell="$out/zell" zell_notrace="$out/notrace" go="$out/go"
