#!/bin/bash
# Starts a throwaway PostgreSQL cluster for benchmarks/orm_sqlite (as the
# postgres user when run as root, since postgres refuses to run as root):
#   pg.sh start <dir> <port>   initdb + start (trust auth, fsync on); prints the DSN
#   pg.sh stop <dir>           stop the server and delete the cluster
set -e
bin=${PGBIN:-$(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -V | tail -n 1)}
[ -x "$bin/postgres" ] || { echo "pg.sh: no PostgreSQL server binaries (set PGBIN)" >&2; exit 1; }
as() { if [ "$(id -u)" = 0 ]; then runuser -u postgres -- "$@"; else "$@"; fi; }
case $1 in
    start)
        dir=$2; port=$3
        mkdir -p "$dir"
        [ "$(id -u)" = 0 ] && chown postgres "$dir"
        as "$bin/initdb" -D "$dir/data" -U bench --auth=trust > "$dir/initdb.log"
        as "$bin/pg_ctl" -D "$dir/data" -l "$dir/server.log" -o "-p $port -k $dir -c listen_addresses=127.0.0.1" -w start > /dev/null
        echo "postgres://bench@127.0.0.1:$port/postgres?sslmode=disable"
        ;;
    stop)
        dir=$2
        as "$bin/pg_ctl" -D "$dir/data" -m fast -w stop > /dev/null || true
        rm -rf "$dir"
        ;;
    *)
        echo "usage: pg.sh start <dir> <port> | stop <dir>" >&2
        exit 2
        ;;
esac
