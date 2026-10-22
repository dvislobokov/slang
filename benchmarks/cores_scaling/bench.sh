#!/bin/bash
# Builds main.zl and main.go, runs both RUNS times alternating (checksums
# must be equal), prints the best time of every phase and worker count,
# Zell/Go ratios and the speedup from 1 to 4 workers.
# Usage: benchmarks/cores_scaling/bench.sh   (also run by scripts/bench-perf.sh)
# Environment: ZELL=<compiler>, RUNS=N (default 1), SCALE=small.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ZELL=${ZELL:-$root/target/release/zell}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# (2>/dev/null: the W0001 cycle warning on Node is expected)
"$ZELL" build "$here/main.zl" --release -o "$out/zell" 2>/dev/null >/dev/null
(cd "$here" && go build -o "$out/go" main.go)
python3 "$root/scripts/bench-rows.py" "${RUNS:-1}" zell="$out/zell" go="$out/go" | tee "$out/table"
awk '($2 == 1 || $2 == 4) && $3 == "ms" { z[$1, $2] = $4; g[$1, $2] = $5; seen[$1] = 1 }
     END { for (p in seen) if (z[p, 4] > 0 && g[p, 4] > 0)
         printf "speedup 1->4 %-9s zell %.2fx  go %.2fx\n", p, z[p, 1] / z[p, 4], g[p, 1] / g[p, 4] }' "$out/table"
