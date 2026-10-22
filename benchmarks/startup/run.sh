#!/bin/bash
# startup: time from exec to first output, Zell vs Go, N runs each
# (default 300; SCALE=small: 20), programs alternating (harness/main.go):
#   hello      prints one line
#   web_api    the servers of benchmarks/web_api (Zell web, Gin, net/http)
#              until they print their port on stderr (socket listening)
#   cli        reads a 1 MB JSON configuration into typed structs, prints a
#              summary (the summaries must be equal)
# Then the stripped sizes of the web_api servers (Zell --release, Go
# -ldflags="-s -w"); hello sizes come from scripts/bench-size.sh.
# Usage: benchmarks/startup/run.sh
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/benchmarks/startup
ZELL=${ZELL:-$root/target/release/zell}
N=${N:-300}
[ "$SCALE" = small ] && N=20
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"$ZELL" build "$here/hello/main.zl" --release -o "$out/hello.zell"
"$ZELL" build "$here/cli/main.zl" --release -o "$out/cli.zell"
"$ZELL" build "$root/benchmarks/web_api/server.zl" --release -o "$out/web.zell"
(cd "$here" && go build -ldflags="-s -w" -o "$out/hello.go" ./hello && go build -ldflags="-s -w" -o "$out/cli.go" ./cli \
    && go build -o "$out/harness" ./harness)
(cd "$root/benchmarks/web_api/go" && go build -ldflags="-s -w" -o "$out/web.gin" ./gin && go build -ldflags="-s -w" -o "$out/web.nethttp" ./nethttp)
"$out/harness" -genconfig "$out/config.json"
"$out/hello.zell" > "$out/a"; "$out/hello.go" > "$out/b"; cmp "$out/a" "$out/b"
"$out/cli.zell" "$out/config.json" > "$out/a"; "$out/cli.go" "$out/config.json" > "$out/b"
if ! cmp -s "$out/a" "$out/b"; then
    echo "MISMATCH: cli output differs: $(cat "$out/a") / $(cat "$out/b")" >&2
    exit 1
fi
echo "== config $(stat -c %s "$out/config.json") bytes: $(cat "$out/a")"
"$out/harness" -n "$N" \
    "hello.zell=$out/hello.zell" "hello.go=$out/hello.go" \
    "web_api.zell=stderr:$out/web.zell" "web_api.gin=stderr:$out/web.gin" "web_api.nethttp=stderr:$out/web.nethttp" \
    "cli.zell=$out/cli.zell $out/config.json" "cli.go=$out/cli.go $out/config.json"
echo "== stripped sizes (bytes)"
for f in hello.zell hello.go cli.zell cli.go web.zell web.gin web.nethttp; do
    printf '%-14s %10d\n' "$f" "$(stat -c %s "$out/$f")"
done
