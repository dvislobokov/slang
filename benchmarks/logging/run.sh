#!/bin/bash
# logging: log (main.zl) vs zap, zerolog and slog (main.go -impl=...).
# RUNS rounds (default 5), loggers alternating; every run's stdout must
# equal Zell's and its log file must pass verify.py (same records and
# properties); the file is deleted right after (5M records are about 1.2 GB
# per logger, so keep that much free in $LOGDIR, default /tmp).
# Prints every run's "scenario <name> <records/s> <allocs/record>" and the
# best records/s per scenario.
# Usage: benchmarks/logging/run.sh   (SCALE=small for a smoke test)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/benchmarks/logging
ZELL=${ZELL:-$root/target/release/zell}
RUNS=${RUNS:-5}
LOGDIR=${LOGDIR:-/tmp}
out=$(mktemp -d)
log="$LOGDIR/zell-bench-$$.log"
trap 'rm -rf "$out" "$log"' EXIT
"$ZELL" build "$here/main.zl" --release -o "$out/zell"
(cd "$here" && go build -o "$out/go" main.go)
n=5000000
[ "$SCALE" = small ] && n=100000
n=${RECORDS:-$n}

variants="zell zap zerolog slog"
for ((i = 1; i <= RUNS; i++)); do
    for v in $variants; do
        if [ $v = zell ]; then bin=("$out/zell"); else bin=("$out/go" "-impl=$v"); fi
        "${bin[@]}" "$log" > "$out/stdout.$v" 2> "$out/scen.$v.$i"
        if [ $v = zell ]; then cp "$out/stdout.zell" "$out/ref"; elif ! cmp -s "$out/ref" "$out/stdout.$v"; then
            echo "MISMATCH: stdout of $v differs from zell" >&2
            exit 1
        fi
        python3 "$here/verify.py" "$log" "$n" > /dev/null
        rm -f "$log"
        echo "run $i $v: $(tr '\n' ' ' < "$out/scen.$v.$i")"
    done
done
echo "== best of $RUNS: records/s (allocs/record), $n records"
for v in $variants; do
    cat "$out"/scen.$v.* | awk -v v=$v '
        { if (!($2 in best) || $3 > best[$2]) { best[$2] = $3; al[$2] = $4 } if (!($2 in seen)) { seen[$2] = 1; order[k++] = $2 } }
        END { printf "%-8s", v; for (i = 0; i < k; i++) printf " %s=%s (%s)", order[i], best[order[i]], al[order[i]]; print "" }'
done
