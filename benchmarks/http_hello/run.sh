#!/bin/bash
# Cross-measures HTTP servers with ONE load generator (the Zell one, and
# the Go one), each server in its own process:
#   zell load -> zell server, zell load -> go server,
#   go load    -> zell server, go load    -> go server,
# then HTTP/2 over TLS with Go's load generator (64 streams on one
# connection, http.Transport{ForceAttemptHTTP2}):
#   go h2 load -> zell server (--server-tls), go h2 load -> go server (-server-tls).
# RUNS rounds (default 1), the pairs alternating within a round; after
# every load run the server's CPU time per request (user+sys, from
# /proc/<pid>/stat) is printed, and at the end each server's peak RSS.
# Usage: benchmarks/http_hello/run.sh   (needs target/release/zell and go)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
ZELL=${ZELL:-$root/target/release/zell}
RUNS=${RUNS:-1}
out=$(mktemp -d)
# Servers are started in this shell (not in a $(...) subshell) and killed
# by PID, so none outlives the script.
declare -A pid port
cleanup() { kill "${pid[@]}" 2>/dev/null || true; rm -rf "$out"; }
trap cleanup EXIT
"$ZELL" build "$root/benchmarks/http_hello/main.zl" --release -o "$out/sl"
(cd "$root/benchmarks/http_hello" && go build -o "$out/go" main.go)
start() { # name binary flag: starts the server, sets pid[name] and port[name]
    "$2" "$3" > /dev/null 2> "$out/port.$1" &
    pid[$1]=$!
    for _ in $(seq 100); do [ -s "$out/port.$1" ] && break; sleep 0.05; done
    port[$1]=$(head -n 1 "$out/port.$1")
}
cpu() { # pid -> user+sys clock ticks
    awk '{ print $14 + $15 }' "/proc/$1/stat"
}
measure() { # server label load-command...: runs the load, prints req/s and server CPU/request
    local s=$1 label=$2; shift 2
    local c0 c1
    c0=$(cpu "${pid[$s]}")
    "$@" > "$out/stdout" 2> "$out/stderr"
    c1=$(cpu "${pid[$s]}")
    local n rps
    n=$(awk '/^requests/ { print $2 }' "$out/stdout")
    rps=$(grep -o '[0-9.]* req/s' "$out/stderr" | tail -1)
    echo "$label: $rps, server CPU $(python3 -c "print(f'{($c1 - $c0) / $(getconf CLK_TCK) / $n * 1e6:.1f}')") us/request ($n requests)"
}
start zell "$out/sl" --server
start go "$out/go" -server
start zell_tls "$out/sl" --server-tls
start go_tls "$out/go" -server-tls
for ((i = 1; i <= RUNS; i++)); do
    echo "== round $i"
    for s in zell go; do
        measure $s "zell load -> $s server" "$out/sl" --load "127.0.0.1:${port[$s]}"
        measure $s "go load   -> $s server" "$out/go" -load "127.0.0.1:${port[$s]}"
    done
    for s in zell go; do
        measure ${s}_tls "go h2 load -> $s server (HTTP/2 over TLS)" "$out/go" -load2 "127.0.0.1:${port[${s}_tls]}"
    done
done
for s in zell go zell_tls go_tls; do
    echo "== $s server peak RSS $(awk '/VmHWM/ { print $2 }' "/proc/${pid[$s]}/status") kB"
done
