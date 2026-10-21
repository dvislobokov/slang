#!/bin/sh
# Installs a Slang toolchain on Linux or macOS (docs/PLATFORMS.md, "Установка"):
#
#   curl -fsSL https://raw.githubusercontent.com/dvislobokov/slang/master/scripts/install.sh | sh
#   sh install.sh [--version X.Y.Z] [--dir ~/.slang/toolchain] [--from slang-X-host.tar.gz]
#
# Downloads slang-<version>-<host>.tar.gz from $SLANG_DIST_URL (a directory
# of release files; default: the GitHub release of that version, or of the
# latest one, whose VERSION file names it), checks its .sha256, unpacks it into
# <dir>/slang-<version>-<host>/ and points <dir>/current at it, then links
# bin/slang and bin/slang-lsp into ~/.local/bin (or $SLANG_BIN_DIR).
# Linking Slang programs needs clang on PATH (and lld for cross targets).
set -eu
version=${SLANG_VERSION:-latest}
dir=${SLANG_TOOLCHAIN_DIR:-$HOME/.slang/toolchain}
bindir=${SLANG_BIN_DIR:-$HOME/.local/bin}
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
    if [ -n "${SLANG_DIST_URL:-}" ]; then
        url=$SLANG_DIST_URL             # a directory of release files (VERSION, archives)
    elif [ "$version" = latest ]; then
        url=https://github.com/dvislobokov/slang/releases/latest/download
    else
        url=https://github.com/dvislobokov/slang/releases/download/v$version
    fi
    if [ "$version" = latest ]; then
        fetch "$url/VERSION" "$tmp/VERSION" || { echo "install.sh: cannot read $url/VERSION; pass --version" >&2; exit 1; }
        version=$(cat "$tmp/VERSION")
    fi
    archive="$tmp/slang-$version-$host.tar.gz"
    echo "downloading $url/slang-$version-$host.tar.gz"
    fetch "$url/slang-$version-$host.tar.gz" "$archive"
    if fetch "$url/slang-$version-$host.tar.gz.sha256" "$tmp/sum" 2>/dev/null; then
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
ln -sf "$dir/current/bin/slang" "$bindir/slang"
ln -sf "$dir/current/bin/slang-lsp" "$bindir/slang-lsp"
echo "installed $name in $dir (current -> $name); slang and slang-lsp linked into $bindir"
case ":$PATH:" in *":$bindir:"*) ;; *) echo "add $bindir to PATH:  export PATH=\"$bindir:\$PATH\"" ;; esac
command -v clang >/dev/null 2>&1 || echo "note: linking needs clang (apt install clang lld / xcode-select --install)"
