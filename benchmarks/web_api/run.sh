#!/bin/bash
# web_api: the `web` framework (server.zl) vs Gin (go/gin) and plain
# net/http + encoding/json (go/nethttp), each server in its own process,
# all loaded by the same Go generator (go/load: 64 keep-alive connections,
# 200k requests mixed over 4 endpoints; req/s, p50, p99).
#
# For every server: `load -check` on the fresh server (statuses + checksum
# of a fixed sequence, must be equal for all servers), then RUNS rounds of
# the load (servers alternate within a round; the checksum of every load
# run must be equal too). Prints one "result" line per run.
#
# Usage: benchmarks/web_api/run.sh
# Environment: ZELL (default target/release/zell), RUNS (default 5),
#   SCALE=small (4000 requests, for a smoke test), CONNS, REQUESTS.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/benchmarks/web_api
ZELL=${ZELL:-$root/target/release/zell}
RUNS=${RUNS:-5}
out=$(mktemp -d)
# Servers are started in this shell (not in a $(...) subshell) and
# killed by PID, so none outlives the script.
declare -A pid port
cleanup() { kill "${pid[@]}" 2>/dev/null || true; rm -rf "$out"; }
trap cleanup EXIT
"$ZELL" build "$here/server.zl" --release -o "$out/zell"
(cd "$here/go" && go build -o "$out/gin" ./gin && go build -o "$out/nethttp" ./nethttp && go build -o "$out/load" ./load)

start() { # name: starts the server, sets pid[name] and port[name]
    "$out/$1" > /dev/null 2> "$out/port.$1" &
    pid[$1]=$!
    for _ in $(seq 100); do [ -s "$out/port.$1" ] && break; sleep 0.05; done
    port[$1]=$(head -n 1 "$out/port.$1")
}

servers="zell gin nethttp"
for s in $servers; do
    start "$s"
    "$out/load" -check "127.0.0.1:${port[$s]}" > "$out/check.$s"
    echo "== $s check: $(tr '\n' ' ' < "$out/check.$s")"
done
for s in $servers; do
    if ! cmp -s "$out/check.zell" "$out/check.$s"; then
        echo "MISMATCH: check output of $s differs from zell" >&2
        exit 1
    fi
done
for ((i = 1; i <= RUNS; i++)); do
    for s in $servers; do
        "$out/load" "127.0.0.1:${port[$s]}" > "$out/load.$s" 2>/dev/null
        sum=$(grep '^requests' "$out/load.$s")
        echo "run $i $s: $(grep '^result' "$out/load.$s") ($sum)"
        if [ "$s" = zell ]; then ref=$sum; elif [ "$sum" != "$ref" ]; then
            echo "MISMATCH: load checksum of $s differs from zell" >&2
            exit 1
        fi
    done
done
for s in $servers; do
    rss=$(awk '/VmHWM/ {print $2}' "/proc/${pid[$s]}/status" 2>/dev/null || echo "?")
    echo "== $s peak RSS ${rss} kB"
done
