#!/bin/bash
# orm_sqlite: data (main.zl) vs GORM and database/sql (main.go -impl=...),
# on SQLite in memory and in a file (under $TMPDIR), and on PostgreSQL
# when its server binaries are installed (pg.sh starts a throwaway cluster
# on PGPORT, default 55433; PG=0 skips it). RUNS rounds (default 5),
# implementations alternating within a round; every run's stdout (the
# checksums) must equal Zell's. Prints the phase times of every run
# ("phase <name> <ms>") and the best of each phase per variant.
# Usage: benchmarks/orm_sqlite/run.sh   (SCALE=small for a smoke test)
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
here=$root/benchmarks/orm_sqlite
ZELL=${ZELL:-$root/target/release/zell}
RUNS=${RUNS:-5}
out=$(mktemp -d)
pgdir=""
cleanup() {
    [ -n "$pgdir" ] && { "$here/pg.sh" stop "$pgdir/c"; rm -rf "$pgdir"; }
    rm -rf "$out"
}
trap cleanup EXIT
"$ZELL" build "$here/main.zl" --release -o "$out/zell"
(cd "$here" && go build -o "$out/go" main.go)

stores="memory file"
if [ "${PG:-1}" != 0 ] && ls /usr/lib/postgresql/*/bin/postgres > /dev/null 2>&1; then
    pgdir=$(mktemp -d /tmp/zell-bench-pg.XXXXXX)
    chmod 755 "$pgdir"
    pgdsn=$("$here/pg.sh" start "$pgdir/c" "${PGPORT:-55433}")
    stores="$stores postgres"
else
    echo "== PostgreSQL: no server binaries (or PG=0), SQLite only"
fi

variants="zell gorm sql"
cmd() { # variant -> command
    case $1 in
        zell) echo "$out/zell" ;;
        gorm) echo "$out/go -impl=gorm" ;;
        sql) echo "$out/go -impl=sql" ;;
    esac
}
for store in $stores; do
    for ((i = 1; i <= RUNS; i++)); do
        for v in $variants; do
            dsn=""
            [ $store = file ] && dsn="$out/$v.db"
            [ $store = postgres ] && dsn=$pgdsn
            $(cmd $v) $dsn > "$out/stdout.$v" 2> "$out/phases.$v.$i"
            if [ $v = zell ]; then cp "$out/stdout.zell" "$out/ref"; elif ! cmp -s "$out/ref" "$out/stdout.$v"; then
                echo "MISMATCH: $v ($store) differs from zell:" >&2
                diff "$out/ref" "$out/stdout.$v" >&2
                exit 1
            fi
            echo "run $i $store $v: $(tr '\n' ' ' < "$out/phases.$v.$i")"
        done
    done
    echo "== $store, best of $RUNS (ms); checksums: $(tr '\n' ' ' < "$out/ref")"
    for v in $variants; do
        cat "$out"/phases.$v.* | awk -v v=$v '
            { if (!($2 in best) || $3 < best[$2]) best[$2] = $3; if (!($2 in seen)) { seen[$2] = 1; order[n++] = $2 } }
            END { printf "%-5s", v; for (i = 0; i < n; i++) printf " %s=%s", order[i], best[order[i]]; print "" }'
    done
    rm -f "$out"/phases.*
done
