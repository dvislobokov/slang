#!/bin/sh
# Prints a markdown table of the spec corpus (tests/spec) by area:
# number of tests, how many are passing / pending (from their `// status:`
# directive) and how many of them are compile-fail tests (`// error:`).
# A test is a .sl file directly in an area directory, or a subdirectory
# (multi-file test; its directives are read from its first .sl file).
#
# Usage: scripts/spec-status.sh [spec-dir]   (default: tests/spec)
set -eu
root=${1:-$(dirname "$0")/../tests/spec}

# Directive value of KEY in the leading // comment block of FILE.
directive() {
    awk -v key="$2" '
        !/^[[:space:]]*\/\// { exit }
        { line = $0; sub(/^[[:space:]]*\/\/[[:space:]]*/, "", line)
          if (index(line, key ":") == 1) { sub(key ":[[:space:]]*", "", line); print line; exit } }
    ' "$1"
}

echo "| Area | Tests | Passing | Pending | Compile-fail |"
echo "|------|------:|--------:|--------:|-------------:|"
T=0; P=0; N=0; C=0
for dir in "$root"/*/; do
    area=$(basename "$dir")
    t=0; p=0; n=0; c=0
    for entry in "$dir"*; do
        if [ -d "$entry" ]; then
            file=$(ls "$entry"/*.sl 2>/dev/null | sort | head -n 1)
            [ -n "$file" ] || continue
        else
            case "$entry" in *.sl) file=$entry ;; *) continue ;; esac
        fi
        t=$((t + 1))
        case "$(directive "$file" status)" in
            passing) p=$((p + 1)) ;;
            pending) n=$((n + 1)) ;;
        esac
        [ -n "$(directive "$file" error)" ] && c=$((c + 1))
    done
    echo "| $area | $t | $p | $n | $c |"
    T=$((T + t)); P=$((P + p)); N=$((N + n)); C=$((C + c))
done
echo "| **total** | **$T** | **$P** | **$N** | **$C** |"
