#!/bin/bash
# Cross-measures HTTP servers with ONE load generator (the Zell one, and
# the Go one), each server in its own process:
#   zell load -> zell server, zell load -> go server,
#   go load    -> zell server, go load    -> go server,
# then HTTP/2 over TLS with Go's load generator (64 streams on one
# connection, http.Transport{ForceAttemptHTTP2}):
#   go h2 load -> zell server (--server-tls), go h2 load -> go server (-server-tls).
# Usage: benchmarks/http_hello/run.sh   (needs target/release/zell and go)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
ZELL=${ZELL:-$root/target/release/zell}
out=$(mktemp -d)
cleanup() { kill $(jobs -p) 2>/dev/null || true; rm -rf "$out"; }
trap cleanup EXIT
"$ZELL" build "$root/benchmarks/http_hello/main.zl" --release -o "$out/sl"
(cd "$root/benchmarks/http_hello" && go build -o "$out/go" main.go)
start() { # binary flag -> port
    "$1" "$2" > /dev/null 2> "$out/port.$3" &
    for _ in $(seq 100); do [ -s "$out/port.$3" ] && break; sleep 0.05; done
    cat "$out/port.$3"
}
sp=$(start "$out/sl" --server sl)
gp=$(start "$out/go" -server go)
for srv in "zell:$sp" "go:$gp"; do
    name=${srv%%:*}; port=${srv#*:}
    echo "== $name server"
    "$out/sl" --load "127.0.0.1:$port" >/dev/null
    "$out/go" -load "127.0.0.1:$port" >/dev/null
done
sp2=$(start "$out/sl" --server-tls sl2)
gp2=$(start "$out/go" -server-tls go2)
for srv in "zell:$sp2" "go:$gp2"; do
    name=${srv%%:*}; port=${srv#*:}
    echo "== $name server, HTTP/2 over TLS"
    "$out/go" -load2 "127.0.0.1:$port" >/dev/null
done
