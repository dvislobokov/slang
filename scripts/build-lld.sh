#!/bin/sh
# Builds bin/lld of the self-contained toolchain (docs/PLATFORMS.md,
# "Дистрибутив"): one multi-flavour lld (ELF, COFF/MinGW, Mach-O) that
# depends on nothing but the OS's C library, from LLVM 18.
#
#   scripts/build-lld.sh [--out DIR]        -> DIR/lld (default build/lld/)
#
# How, in order:
#   1. $LLD_BINARY: an existing self-contained lld to copy (Windows: lld.exe of
#      the official clang+llvm-18.1.8-x86_64-pc-windows-msvc tarball, linked
#      with the MSVC runtime that package-toolchain.sh ships next to it).
#   2. The static lld and LLVM libraries of an installed LLVM 18 ($LLVM_CONFIG,
#      llvm-config-18 or llvm-config: Ubuntu's llvm-18-dev + liblld-18-dev,
#      Homebrew's llvm@18): scripts/lld/lld-main.cpp (lld's own driver
#      reduced to its four flavours) linked statically, libstdc++/zlib/zstd/
#      terminfo included, the backends other than X86 and AArch64 left out.
#   3. Otherwise lld built from the llvm-project 18.1.8 sources (cmake +
#      ninja, X86 and AArch64 only, no zlib/zstd/libxml2/terminfo): slow
#      (30-60 minutes), meant for a cached CI step on hosts without (2).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/build/lld
while [ $# -gt 0 ]; do
    case "$1" in
        --out) shift; out=$1 ;;
        *) echo "build-lld.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
mkdir -p "$out"
exe=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) exe=.exe ;; esac
dst=$out/lld$exe

if [ -n "${LLD_BINARY:-}" ]; then
    cp "$LLD_BINARY" "$dst"
    "$dst" -flavor gnu --version
    exit 0
fi

LC=${LLVM_CONFIG:-$(command -v llvm-config-18 || command -v llvm-config || true)}
if [ -n "$LC" ] && [ -f "$("$LC" --libdir)/liblldELF.a" ]; then
    libdir=$("$LC" --libdir)
    CXX=${CXX:-$("$LC" --bindir)/clang++}
    [ -x "$CXX" ] || CXX=$(command -v clang++-18 || command -v clang++ || command -v c++)
    stubs=""
    for t in $("$LC" --targets-built); do
        case "$t" in X86|AArch64) ;; *) stubs="$stubs ZELL_STUB_TARGET($t)" ;; esac
    done
    case "$(uname -s)" in
        Darwin)
            # system libz/libc++/libcurses only; Homebrew's zstd linked in statically
            zstd=$(brew --prefix zstd 2>/dev/null)/lib/libzstd.a
            [ -f "$zstd" ] || zstd=-lzstd
            sys="$zstd -lz -lcurses -lm"
            static="" ;;
        *)
            sys="-Wl,-Bstatic -lz -lzstd -ltinfo -Wl,-Bdynamic -lrt -ldl -lm -lpthread"
            static="-static-libstdc++ -static-libgcc -Wl,--gc-sections -Wl,-O1" ;;
    esac
    # shellcheck disable=SC2086
    "$CXX" -O2 -std=c++17 $("$LC" --cxxflags) "-DZELL_LLD_STUB_TARGETS=$stubs" \
        "$root/scripts/lld/lld-main.cpp" -o "$dst" -L"$libdir" \
        -llldMinGW -llldCOFF -llldELF -llldMachO -llldCommon $("$LC" --link-static --libs all) \
        $static $sys
    strip "$dst" 2>/dev/null || true
else
    # 3. from source
    v=18.1.8
    sha=0b58557a6d32ceee97c8d533a59b9212d87e0fc4d2833924eb6c611247db2f2a
    src=$root/build/lld-src
    mkdir -p "$src"
    tarball=$src/llvm-project-$v.src.tar.xz
    if [ ! -f "$tarball" ]; then
        curl -fsSL --retry 3 -o "$tarball.part" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$v/llvm-project-$v.src.tar.xz"
        mv "$tarball.part" "$tarball"
    fi
    got=$( (sha256sum "$tarball" 2>/dev/null || shasum -a 256 "$tarball") | cut -d' ' -f1)
    [ "$got" = "$sha" ] || { echo "build-lld.sh: $tarball: sha256 $got, expected $sha" >&2; exit 1; }
    [ -d "$src/llvm-project-$v.src" ] || tar xJf "$tarball" -C "$src" "llvm-project-$v.src/llvm" "llvm-project-$v.src/lld" "llvm-project-$v.src/cmake" "llvm-project-$v.src/libunwind/include" "llvm-project-$v.src/third-party"
    b=$src/build
    cmake -G Ninja -S "$src/llvm-project-$v.src/llvm" -B "$b" -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_PROJECTS=lld -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
        -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
        -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_ENABLE_BINDINGS=OFF \
        -DLLVM_BUILD_TOOLS=OFF -DLLVM_STATIC_LINK_CXX_STDLIB=ON
    ninja -C "$b" lld
    cp "$b/bin/lld$exe" "$dst"
    strip "$dst" 2>/dev/null || true
fi
"$dst" -flavor gnu --version
ls -l "$dst"
