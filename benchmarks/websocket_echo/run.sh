#!/bin/bash
# websocket_echo: net.ws vs gorilla/websocket, cross-measured like
# http_hello: each client (Zell, Go) against each server (Zell, Go), every
# server in its own process; RUNS rounds (default 5), pairs alternating.
# Every client's stdout must be the same ("messages <n> bytes <n>").
# Needs ulimit -n >= 2 * CONNS + 100 (1000 connections by default).
# Usage: benchmarks/websocket_echo/run.sh   (SCALE=small for a smoke test)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/benchmarks/websocket_echo
ZELL=${ZELL:-$root/target/release/zell}
RUNS=${RUNS:-5}
out=$(mktemp -d)
cleanup() { kill $(jobs -p) 2>/dev/null || true; rm -rf "$out"; }
trap cleanup EXIT
"$ZELL" build "$here/main.zl" --release -o "$out/zell"
(cd "$here" && go build -o "$out/go" main.go)
start() { # binary flag name -> port
    "$1" "$2" > /dev/null 2> "$out/port.$3" &
    for _ in $(seq 100); do [ -s "$out/port.$3" ] && break; sleep 0.05; done
    head -n 1 "$out/port.$3"
}
zp=$(start "$out/zell" --server zell)
gp=$(start "$out/go" -server go)
ref=""
for ((i = 1; i <= RUNS; i++)); do
    for srv in "zell:$zp" "go:$gp"; do
        name=${srv%%:*}; port=${srv#*:}
        url="ws://127.0.0.1:$port/ws"
        for cl in zell go; do
            if [ $cl = zell ]; then "$out/zell" --load "$url" > "$out/stdout" 2> "$out/stderr"
            else "$out/go" -load "$url" > "$out/stdout" 2> "$out/stderr"; fi
            res=$(cat "$out/stdout")
            [ -z "$ref" ] && ref=$res
            if [ "$res" != "$ref" ]; then
                echo "MISMATCH: $cl client -> $name server: $res (want $ref)" >&2
                exit 1
            fi
            echo "run $i: $cl client -> $name server: $(cat "$out/stderr")"
        done
    done
done
echo "== all clients: $ref"
