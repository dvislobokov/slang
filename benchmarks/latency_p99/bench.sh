#!/bin/bash
# Latency distribution of the Zell and Go HTTP/1.1 servers under Go's load
# generator (main.go -load: 64 keep-alive connections, 200k GET / in all,
# p50/p90/p99/p999/max in microseconds), each server in its own process:
#   idle   the server alone (baseline)
#   churn  the server plus a background task that keeps rebuilding a
#          ~200 MB (Go live heap) heap of small objects, lists and maps
# First checks that both churn self-checks print the same checksums.
# Usage: benchmarks/latency_p99/bench.sh   (also run by scripts/bench-perf.sh)
# Environment: ZELL=<compiler>, RUNS=N (runs per server and mode, servers
#   alternating; the run with the lowest p99 is printed), SCALE=small.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ZELL=${ZELL:-$root/target/release/zell}
RUNS=${RUNS:-1}
out=$(mktemp -d)
pid=
cleanup() { [ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$out"; }
trap cleanup EXIT
"$ZELL" build "$here/main.zl" --release -o "$out/zell" 2>/dev/null >/dev/null
(cd "$here" && go build -o "$out/go" main.go)

"$out/zell" > "$out/zell.chk" 2>/dev/null
"$out/go" > "$out/go.chk" 2>/dev/null
if ! cmp -s "$out/zell.chk" "$out/go.chk"; then
    echo "latency_p99: churn checksums differ" >&2
    diff "$out/zell.chk" "$out/go.chk" >&2
    exit 1
fi
echo "latency_p99 churn self-check: $(tr '\n' ' ' < "$out/zell.chk")(equal)"

start() { # binary flag tag -> sets pid and port
    "$1" "$2" > /dev/null 2> "$out/err.$3" &
    pid=$!
    for _ in $(seq 1200); do [ -s "$out/err.$3" ] && break; sleep 0.05; done
    port=$(head -1 "$out/err.$3")
}

for ((i = 0; i < RUNS; i++)); do
    for mode in idle churn; do
        for srv in zell go; do
            if [ $srv = zell ]; then flag=--server; else flag=-server; fi
            [ $mode = churn ] && flag=$flag-churn
            start "$out/$srv" "$flag" "$srv.$mode"
            "$out/go" -load "127.0.0.1:$port" > "$out/load.out" 2> "$out/load.err"
            kill "$pid"; wait "$pid" 2>/dev/null || true; pid=
            churn=$(awk '$1 == "churn" { s += $2; n++ } END { if (n) printf "%d", s / n; else print "-" }' "$out/err.$srv.$mode")
            if [ -s "$out/ref.out" ]; then
                cmp -s "$out/ref.out" "$out/load.out" || { echo "latency_p99: load output differs for $srv $mode" >&2; exit 1; }
            else
                cp "$out/load.out" "$out/ref.out"
            fi
            echo "$srv $mode $(cat "$out/load.err") churn/s=$churn" >> "$out/runs"
        done
    done
done
echo "latency_p99 load: $(cat "$out/ref.out") (equal for every run)"
# best run (lowest p99) per server and mode
python3 - "$out/runs" <<'PY'
import sys
best = {}
for line in open(sys.argv[1]):
    f = line.split()
    kv = dict(x.split("=", 1) for x in f[3:])
    key = (f[0], f[1])
    if key not in best or float(kv["p99"]) < float(best[key]["p99"]):
        best[key] = kv
print(f"{'server':<6} {'mode':<6} {'req/s':>8} {'p50_us':>7} {'p90_us':>7} {'p99_us':>7} {'p999_us':>8} {'max_us':>8} {'churn/s':>8}")
for mode in ("idle", "churn"):
    for srv in ("zell", "go"):
        kv = best.get((srv, mode))
        if kv:
            print(f"{srv:<6} {mode:<6} {kv['req/s']:>8} {kv['p50']:>7} {kv['p90']:>7} {kv['p99']:>7} {kv['p999']:>8} {kv['max']:>8} {kv['churn/s']:>8}")
PY
