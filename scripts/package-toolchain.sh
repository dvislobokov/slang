#!/bin/sh
# Packages a Zell toolchain for this machine (docs/PLATFORMS.md, "Установка"):
#
#   dist/zell-<version>-<host>.tar.gz      unpacks to zell-<version>-<host>/:
#       bin/zell, bin/zell-lsp            the compiler and the language server
#       bin/zell-mcp                        the MCP server for AI agents (agents/)
#       std/                                standard library sources (IDE navigation;
#                                           the compiler also embeds them)
#       targets/<host triple>/libzell_rt.a the runtime for this machine
#       tools/                              lldb/gdb formatters, VS Code client
#       README.md, VERSION
#   dist/zell-runtime-<version>-<triple>.tar.gz
#                                           one per runtime in runtime/build/<triple>/
#                                           (cross-built: make -C runtime CROSS=...),
#                                           unpacking to <triple>/libzell_rt.a: what
#                                           `zell target add <triple>` downloads
#   *.sha256 next to every archive
#
# Usage: scripts/package-toolchain.sh [--no-build] [--out dir]
#   --no-build  use the existing target/release binaries and runtime archives
#   --out dir   output directory (default dist/)
# The archives are what a release uploads; install.sh / install.ps1 install
# them, and ZELL_TARGETS_URL may point at a directory of runtime archives.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/dist"
build=yes
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) build=no ;;
        --out) shift; out=$1 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
version=$(sed -n 's/^version *= *"\(.*\)"/\1/p' "$root/Cargo.toml" | head -1)
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) host=linux-amd64; triple=x86_64-unknown-linux-gnu ;;
    Linux-aarch64) host=linux-arm64; triple=aarch64-unknown-linux-gnu ;;
    Darwin-arm64) host=macos-arm64; triple=aarch64-apple-darwin ;;
    Darwin-x86_64) host=macos-amd64; triple=x86_64-apple-darwin ;;
    MINGW*-x86_64|MSYS*-x86_64) host=windows-amd64; triple=x86_64-pc-windows-gnu ;;
    *) echo "unsupported host $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
exe=""
[ "$host" = windows-amd64 ] && exe=.exe
if [ "$build" = yes ]; then
    (cd "$root" && cargo build --release --bin zell --bin zell-lsp --bin zell-mcp)
    make -C "$root/runtime" -j4
fi
rt="$root/runtime/libzell_rt.a"
[ "$host" = windows-amd64 ] && rt="$root/runtime/build/x86_64-pc-windows-gnu/libzell_rt.a"
for f in "$root/target/release/zell$exe" "$root/target/release/zell-lsp$exe" "$root/target/release/zell-mcp$exe" "$rt"; do
    [ -f "$f" ] || { echo "missing $f (run without --no-build)" >&2; exit 1; }
done
name="zell-$version-$host"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
d="$stage/$name"
mkdir -p "$d/bin" "$d/targets/$triple" "$d/tools"
cp "$root/target/release/zell$exe" "$root/target/release/zell-lsp$exe" "$root/target/release/zell-mcp$exe" "$d/bin/"
strip "$d/bin/zell$exe" "$d/bin/zell-lsp$exe" "$d/bin/zell-mcp$exe" 2>/dev/null || true
cp -R "$root/std" "$d/std"
cp "$rt" "$d/targets/$triple/libzell_rt.a"
cp -R "$root/tools/." "$d/tools/"
if [ -d "$root/editors/vscode" ]; then
    mkdir -p "$d/tools/vscode"
    (cd "$root/editors/vscode" && tar cf - --exclude=node_modules .) | (cd "$d/tools/vscode" && tar xf -)
fi
printf '%s\n' "$version" > "$d/VERSION"
cat > "$d/README.md" <<README
# Zell $version for $host

    export PATH="\$PWD/bin:\$PATH"     # or run install.sh, which does this
    zell run hello.zl

Linking needs clang (and lld for cross targets) on PATH. Other targets:
\`zell target list\`, \`zell target add windows-amd64\` (downloads into
~/.zell/targets; ZELL_TARGETS_URL overrides the source). See
docs/PLATFORMS.md in the repository.
README
mkdir -p "$out"
sum() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi; }
(cd "$stage" && tar czf "$out/$name.tar.gz" "$name")
(cd "$out" && sum "$name.tar.gz" > "$name.tar.gz.sha256")
printf '%s\n' "$version" > "$out/VERSION"
echo "$out/$name.tar.gz ($(du -k "$out/$name.tar.gz" | cut -f1) KB)"
# runtime archives for zell target add
for lib in "$root"/runtime/build/*/libzell_rt.a; do
    [ -f "$lib" ] || continue
    t=$(basename "$(dirname "$lib")")
    case "$t" in *-unknown-*|*-pc-*|*-apple-*) ;; *) continue ;; esac   # LLVM triples only (not win64/, aarch64-linux-gnu/ object dirs)
    case "$t" in *stubcheck*) continue ;; esac  # macOS stub-SDK compile checks are not runtimes
    rd="$stage/rt-$t"
    mkdir -p "$rd/$t"
    cp "$lib" "$rd/$t/libzell_rt.a"
    (cd "$rd" && tar czf "$out/zell-runtime-$version-$t.tar.gz" "$t")
    (cd "$out" && sum "zell-runtime-$version-$t.tar.gz" > "zell-runtime-$version-$t.tar.gz.sha256")
    echo "$out/zell-runtime-$version-$t.tar.gz"
done
# the host runtime too, so every release lists all tier-1 targets
if [ ! -f "$out/zell-runtime-$version-$triple.tar.gz" ]; then
    rd="$stage/rt-$triple"
    mkdir -p "$rd/$triple"
    cp "$rt" "$rd/$triple/libzell_rt.a"
    (cd "$rd" && tar czf "$out/zell-runtime-$version-$triple.tar.gz" "$triple")
    (cd "$out" && sum "zell-runtime-$version-$triple.tar.gz" > "zell-runtime-$version-$triple.tar.gz.sha256")
    echo "$out/zell-runtime-$version-$triple.tar.gz"
fi
