#!/bin/sh
# Builds the documentation site into site/ and serves it on
# http://localhost:${PORT:-8000}/ (python3 -m http.server).
# Usage: scripts/docs-serve.sh [port]
set -e
cd "$(dirname "$0")/.."
PORT="${1:-${PORT:-8000}}"
ZELL="${ZELL:-target/release/zell}"
if [ ! -x "$ZELL" ]; then
    cargo build --release -p zell
fi
rm -rf site
"$ZELL" doc --site site
echo "serving http://localhost:$PORT/"
exec python3 -m http.server "$PORT" --directory site
