#!/bin/bash
# Runs benchmarks/*/main.{sl,go}, prints wall time, user+sys CPU and peak RSS.
# Usage: ./scripts/bench-perf.sh [bench...]   (default: all)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
SLANG=${SLANG:-$root/target/release/slang}
out=$(mktemp -d)
cd "$root/benchmarks"
benches=${*:-$(ls -d */ | tr -d /)}

measure() { # name binary -> prints "wall user+sys rss_mb"
    local t0 t1 stats
    # /proc-based measurement via bash's times is coarse; use python resource module
    python3 - "$1" <<'PY'
import resource, subprocess, sys, time
t0 = time.perf_counter()
p = subprocess.run([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
wall = time.perf_counter() - t0
ru = resource.getrusage(resource.RUSAGE_CHILDREN)
cpu = ru.ru_utime + ru.ru_stime
rss = ru.ru_maxrss / 1024
print(f"{wall:.2f} {cpu:.2f} {rss:.1f} {p.returncode} {p.stdout.decode().strip()[:30]!r}")
PY
}

printf '%-9s %-6s %8s %8s %8s  %s\n' bench lang wall_s cpu_s rss_mb output
for b in $benches; do
    "$SLANG" build "$b/main.sl" --release -o "$out/$b.sl.bin" >/dev/null
    (cd "$b" && go build -o "$out/$b.go.bin" main.go)
    for lang in sl go; do
        read wall cpu rss rc output < <(measure "$out/$b.$lang.bin")
        printf '%-9s %-6s %8s %8s %8s  %s\n' "$b" "$([ $lang = sl ] && echo slang || echo go)" "$wall" "$cpu" "$rss" "$output"
    done
done
rm -rf "$out"
