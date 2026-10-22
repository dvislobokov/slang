#!/bin/bash
# Runs benchmarks/*/main.{zl,go}, prints wall time, user+sys CPU and peak RSS.
# Usage: ./scripts/bench-perf.sh [bench...]   (default: all)
# Environment: ZELL=<compiler> (default target/release/zell),
#   RUNS=N  run each binary N times (alternating Zell and Go) and print the
#           run with the best wall time (default 1),
#   NOGO=1  skip the Go programs.
#   Extra arguments for a benchmark's binary: benchmarks/<b>/args (one line).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
ZELL=${ZELL:-$root/target/release/zell}
RUNS=${RUNS:-1}
out=$(mktemp -d)
cd "$root/benchmarks"
benches=${*:-$(ls -d */ | tr -d /)}

measure() { # binary [args...] -> prints "wall user+sys rss_mb rc output"
    python3 - "$@" <<'PY'
import os, subprocess, sys, time
t0 = time.perf_counter()
p = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
outp, _ = p.communicate()

wall = time.perf_counter() - t0
import resource
ru = resource.getrusage(resource.RUSAGE_CHILDREN)
print(f"{wall:.3f} {ru.ru_utime + ru.ru_stime:.2f} {ru.ru_maxrss / 1024:.1f} {p.returncode} {outp.decode(errors='replace').strip()[:30]!r}")
PY
}

langs="zl go"
[ -n "$NOGO" ] && langs="zl"
printf '%-12s %-6s %8s %8s %8s  %s\n' bench lang wall_s cpu_s rss_mb output
for b in $benches; do
    [ -f "$b/main.zl" ] || continue
    "$ZELL" build "$b/main.zl" --release -o "$out/$b.zl.bin" >/dev/null
    if [ -z "$NOGO" ]; then (cd "$b" && go build -o "$out/$b.go.bin" main.go); fi
    args=()
    [ -f "$b/args" ] && read -r -a args < "$b/args"
    declare -A best=()
    for ((i = 0; i < RUNS; i++)); do
        for lang in $langs; do
            line=$(measure "$out/$b.$lang.bin" "${args[@]}")
            w=${line%% *}
            if [ -z "${best[$lang]}" ] || python3 -c "import sys; sys.exit(0 if $w < ${best[$lang]%% *} else 1)"; then
                best[$lang]=$line
            fi
        done
    done
    for lang in $langs; do
        read wall cpu rss rc output <<< "${best[$lang]}"
        printf '%-12s %-6s %8s %8s %8s  %s\n' "$b" "$([ $lang = zl ] && echo zell || echo go)" "$wall" "$cpu" "$rss" "$output"
    done
    unset best
done
rm -rf "$out"
