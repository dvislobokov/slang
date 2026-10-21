#!/bin/sh
# Builds the documentation site into site/ and serves it on
# http://localhost:${PORT:-8000}/ (python3 -m http.server).
# Usage: scripts/docs-serve.sh [port]
set -e
cd "$(dirname "$0")/.."
PORT="${1:-${PORT:-8000}}"
SLANG="${SLANG:-target/release/slang}"
if [ ! -x "$SLANG" ]; then
    cargo build --release -p slang
fi
rm -rf site
"$SLANG" doc --site site
echo "serving http://localhost:$PORT/"
exec python3 -m http.server "$PORT" --directory site
