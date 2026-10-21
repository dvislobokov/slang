#!/bin/sh
# Packages a Slang toolchain for this machine (docs/PLATFORMS.md, "Установка"):
#
#   dist/slang-<version>-<host>.tar.gz      unpacks to slang-<version>-<host>/:
#       bin/slang, bin/slang-lsp            the compiler and the language server
#       std/                                standard library sources (IDE navigation;
#                                           the compiler also embeds them)
#       targets/<host triple>/libslang_rt.a the runtime for this machine
#       tools/                              lldb/gdb formatters, VS Code client
#       README.md, VERSION
#   dist/slang-runtime-<version>-<triple>.tar.gz
#                                           one per runtime in runtime/build/<triple>/
#                                           (cross-built: make -C runtime CROSS=...),
#                                           unpacking to <triple>/libslang_rt.a: what
#                                           `slang target add <triple>` downloads
#   *.sha256 next to every archive
#
# Usage: scripts/package-toolchain.sh [--no-build] [--out dir]
#   --no-build  use the existing target/release binaries and runtime archives
#   --out dir   output directory (default dist/)
# The archives are what a release uploads; install.sh / install.ps1 install
# them, and SLANG_TARGETS_URL may point at a directory of runtime archives.
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
    (cd "$root" && cargo build --release --bin slang --bin slang-lsp)
    make -C "$root/runtime" -j4
fi
rt="$root/runtime/libslang_rt.a"
[ "$host" = windows-amd64 ] && rt="$root/runtime/build/x86_64-pc-windows-gnu/libslang_rt.a"
for f in "$root/target/release/slang$exe" "$root/target/release/slang-lsp$exe" "$rt"; do
    [ -f "$f" ] || { echo "missing $f (run without --no-build)" >&2; exit 1; }
done
name="slang-$version-$host"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
d="$stage/$name"
mkdir -p "$d/bin" "$d/targets/$triple" "$d/tools"
cp "$root/target/release/slang$exe" "$root/target/release/slang-lsp$exe" "$d/bin/"
strip "$d/bin/slang$exe" "$d/bin/slang-lsp$exe" 2>/dev/null || true
cp -R "$root/std" "$d/std"
cp "$rt" "$d/targets/$triple/libslang_rt.a"
cp -R "$root/tools/." "$d/tools/"
if [ -d "$root/editors/vscode" ]; then
    mkdir -p "$d/tools/vscode"
    (cd "$root/editors/vscode" && tar cf - --exclude=node_modules .) | (cd "$d/tools/vscode" && tar xf -)
fi
printf '%s\n' "$version" > "$d/VERSION"
cat > "$d/README.md" <<README
# Slang $version for $host

    export PATH="\$PWD/bin:\$PATH"     # or run install.sh, which does this
    slang run hello.sl

Linking needs clang (and lld for cross targets) on PATH. Other targets:
\`slang target list\`, \`slang target add windows-amd64\` (downloads into
~/.slang/targets; SLANG_TARGETS_URL overrides the source). See
docs/PLATFORMS.md in the repository.
README
mkdir -p "$out"
sum() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1"; else shasum -a 256 "$1"; fi; }
(cd "$stage" && tar czf "$out/$name.tar.gz" "$name")
(cd "$out" && sum "$name.tar.gz" > "$name.tar.gz.sha256")
printf '%s\n' "$version" > "$out/VERSION"
echo "$out/$name.tar.gz ($(du -k "$out/$name.tar.gz" | cut -f1) KB)"
# runtime archives for slang target add
for lib in "$root"/runtime/build/*/libslang_rt.a; do
    [ -f "$lib" ] || continue
    t=$(basename "$(dirname "$lib")")
    case "$t" in *-unknown-*|*-pc-*|*-apple-*) ;; *) continue ;; esac   # LLVM triples only (not win64/, aarch64-linux-gnu/ object dirs)
    case "$t" in *stubcheck*) continue ;; esac  # macOS stub-SDK compile checks are not runtimes
    rd="$stage/rt-$t"
    mkdir -p "$rd/$t"
    cp "$lib" "$rd/$t/libslang_rt.a"
    (cd "$rd" && tar czf "$out/slang-runtime-$version-$t.tar.gz" "$t")
    (cd "$out" && sum "slang-runtime-$version-$t.tar.gz" > "slang-runtime-$version-$t.tar.gz.sha256")
    echo "$out/slang-runtime-$version-$t.tar.gz"
done
# the host runtime too, so every release lists all tier-1 targets
if [ ! -f "$out/slang-runtime-$version-$triple.tar.gz" ]; then
    rd="$stage/rt-$triple"
    mkdir -p "$rd/$triple"
    cp "$rt" "$rd/$triple/libslang_rt.a"
    (cd "$rd" && tar czf "$out/slang-runtime-$version-$triple.tar.gz" "$triple")
    (cd "$out" && sum "slang-runtime-$version-$triple.tar.gz" > "slang-runtime-$version-$triple.tar.gz.sha256")
    echo "$out/slang-runtime-$version-$triple.tar.gz"
fi
