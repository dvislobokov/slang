#!/bin/sh
# Claude Code PostToolUse hook (Write|Edit|MultiEdit): for an edited .zl
# file, run `zell fmt` on it and `zell check` on its project (the nearest
# directory with a zell manifest, else the file). A failed check exits 2
# so that Claude sees the errors; everything else exits 0 silently.
#
#   ZELL_AGENT_FORMAT=0   do not format
#   ZELL_AGENT_CHECK=0    do not type-check
#   ZELL_BIN=/path/zell   the compiler (default: zell on PATH)
input=$(cat)
zell=${ZELL_BIN:-zell}
command -v "$zell" >/dev/null 2>&1 || exit 0

# tool_input.file_path, without depending on jq
if command -v jq >/dev/null 2>&1; then
    file=$(printf '%s' "$input" | jq -r '.tool_input.file_path // .tool_input.path // empty' 2>/dev/null)
else
    file=$(printf '%s' "$input" | tr '\n' ' ' | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
fi
case "$file" in
    *.zl) ;;
    *) exit 0 ;;
esac
[ -f "$file" ] || exit 0

if [ "${ZELL_AGENT_FORMAT:-1}" != 0 ]; then
    # syntax errors leave the file alone; the check below reports them
    "$zell" fmt "$file" >/dev/null 2>&1
fi

[ "${ZELL_AGENT_CHECK:-1}" != 0 ] || exit 0
dir=$(dirname "$file")
target=$file
d=$dir
while [ -n "$d" ]; do
    if [ -f "$d/zell.toml" ] || [ -f "$d/zell.json" ] || [ -f "$d/zell.yaml" ] || [ -f "$d/zell.yml" ]; then
        target=$d
        break
    fi
    parent=$(dirname "$d")
    [ "$parent" = "$d" ] && break
    d=$parent
done
case "$file" in
    *_test.zl) exit 0 ;; # test files are checked by `zell test`
esac
out=$("$zell" check "$target" 2>&1) && exit 0
printf 'zell check %s failed:\n%s\n' "$target" "$(printf '%s\n' "$out" | head -n 60)" >&2
exit 2
