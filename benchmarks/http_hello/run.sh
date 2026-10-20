#!/bin/bash
# Cross-measures HTTP servers with ONE load generator (the Slang one, and
# the Go one), each server in its own process:
#   slang load -> slang server, slang load -> go server,
#   go load    -> slang server, go load    -> go server.
# Usage: benchmarks/http_hello/run.sh   (needs target/release/slang and go)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
SLANG=${SLANG:-$root/target/release/slang}
out=$(mktemp -d)
cleanup() { kill $(jobs -p) 2>/dev/null || true; rm -rf "$out"; }
trap cleanup EXIT
"$SLANG" build "$root/benchmarks/http_hello/main.sl" --release -o "$out/sl"
(cd "$root/benchmarks/http_hello" && go build -o "$out/go" main.go)
start() { # binary flag -> port
    "$1" "$2" > /dev/null 2> "$out/port.$3" &
    for _ in $(seq 100); do [ -s "$out/port.$3" ] && break; sleep 0.05; done
    cat "$out/port.$3"
}
sp=$(start "$out/sl" --server sl)
gp=$(start "$out/go" -server go)
for srv in "slang:$sp" "go:$gp"; do
    name=${srv%%:*}; port=${srv#*:}
    echo "== $name server"
    "$out/sl" --load "127.0.0.1:$port" >/dev/null
    "$out/go" -load "127.0.0.1:$port" >/dev/null
done
