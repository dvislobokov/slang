#!/bin/sh
# Installs a Zell toolchain on Linux or macOS (docs/PLATFORMS.md, "Установка"):
#
#   curl -fsSL https://raw.githubusercontent.com/zell-lang/zell/master/scripts/install.sh | sh
#   sh install.sh [--version X.Y.Z] [--dir ~/.zell/toolchain] [--from zell-X-host.tar.gz]
#
# Downloads zell-<version>-<host>.tar.gz from $ZELL_DIST_URL (a directory
# of release files; default: the GitHub release of that version, or of the
# latest one, whose VERSION file names it), checks its .sha256, unpacks it into
# <dir>/zell-<version>-<host>/ and points <dir>/current at it, then links
# bin/zell, bin/zell-lsp and bin/zell-mcp into ~/.local/bin (or $ZELL_BIN_DIR).
# Linking Zell programs needs clang on PATH (and lld for cross targets).
set -eu
version=${ZELL_VERSION:-latest}
dir=${ZELL_TOOLCHAIN_DIR:-$HOME/.zell/toolchain}
bindir=${ZELL_BIN_DIR:-$HOME/.local/bin}
from=""
while [ $# -gt 0 ]; do
    case "$1" in
        --version) shift; version=$1 ;;
        --dir) shift; dir=$1 ;;
        --bin) shift; bindir=$1 ;;
        --from) shift; from=$1 ;;
        *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) host=linux-amd64 ;;
    Linux-aarch64 | Linux-arm64) host=linux-arm64 ;;
    Darwin-arm64) host=macos-arm64 ;;
    Darwin-x86_64) host=macos-amd64 ;;
    *) echo "install.sh: no prebuilt toolchain for $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fetch() { # url file
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"; else wget -q "$1" -O "$2"; fi
}
if [ -n "$from" ]; then
    archive=$from
else
    if [ -n "${ZELL_DIST_URL:-}" ]; then
        url=$ZELL_DIST_URL             # a directory of release files (VERSION, archives)
    elif [ "$version" = latest ]; then
        url=https://github.com/zell-lang/zell/releases/latest/download
    else
        url=https://github.com/zell-lang/zell/releases/download/v$version
    fi
    if [ "$version" = latest ]; then
        fetch "$url/VERSION" "$tmp/VERSION" || { echo "install.sh: cannot read $url/VERSION; pass --version" >&2; exit 1; }
        version=$(cat "$tmp/VERSION")
    fi
    archive="$tmp/zell-$version-$host.tar.gz"
    echo "downloading $url/zell-$version-$host.tar.gz"
    fetch "$url/zell-$version-$host.tar.gz" "$archive"
    if fetch "$url/zell-$version-$host.tar.gz.sha256" "$tmp/sum" 2>/dev/null; then
        want=$(cut -d' ' -f1 < "$tmp/sum")
        got=$( (sha256sum "$archive" 2>/dev/null || shasum -a 256 "$archive") | cut -d' ' -f1)
        [ "$want" = "$got" ] || { echo "install.sh: checksum mismatch ($got, expected $want)" >&2; exit 1; }
    fi
fi
mkdir -p "$dir"
tar xzf "$archive" -C "$dir"
name=$(tar tzf "$archive" | head -1 | cut -d/ -f1)
ln -sfn "$dir/$name" "$dir/current"
mkdir -p "$bindir"
ln -sf "$dir/current/bin/zell" "$bindir/zell"
ln -sf "$dir/current/bin/zell-lsp" "$bindir/zell-lsp"
[ -e "$dir/current/bin/zell-mcp" ] && ln -sf "$dir/current/bin/zell-mcp" "$bindir/zell-mcp"
echo "installed $name in $dir (current -> $name); zell, zell-lsp and zell-mcp linked into $bindir"
case ":$PATH:" in *":$bindir:"*) ;; *) echo "add $bindir to PATH:  export PATH=\"$bindir:\$PATH\"" ;; esac
command -v clang >/dev/null 2>&1 || echo "note: linking needs clang (apt install clang lld / xcode-select --install)"
