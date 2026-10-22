#!/bin/sh
# Smoke test of an unpacked toolchain archive on a clean machine (CI job
# bundle-smoke runs it in a bare ubuntu:24.04 container; docs/PLATFORMS.md,
# "Дистрибутив"): builds examples for every target with nothing but the
# toolchain on PATH, runs what this machine can run and compares the output
# with the examples' .out files.
#
#   scripts/bundle-smoke.sh TOOLCHAIN OUT [--run linux|foreign|none]
#     TOOLCHAIN  the unpacked zell-<version>-<host>/ directory
#     OUT        where the executables go (OUT/<target>/<example>[.exe]);
#                CI hands the macOS and Windows ones to real macOS/Windows runners
#     --run linux    (default) run the linux-amd64 (glibc) and musl executables here
#     --run foreign  also run linux-arm64(-musl) under qemu-aarch64-static and
#                    windows-amd64 under wine (the caller installs them)
#     --run none     only build
# Exit status 1 if anything failed to build or run as expected.
set -eu
tc=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
mode=linux
[ "${3:-}" = --run ] && mode=${4:-linux}
root=$(cd "$(dirname "$0")/.." && pwd)
examples="hello arith control structs enums slices_maps strings interfaces errors defer async channels generics json regex reflection"
host_cpu=$(uname -m)
fail=0
note() { printf '%s\n' "$*"; }
bad() { printf 'FAIL %s\n' "$*"; fail=1; }

# zell sees nothing but the toolchain: PATH is its bin/ only (no compiler,
# linker or C library from the system), a fresh HOME, no ZELL_* settings
mkdir -p "$out/home"
zell() { env -i PATH="$tc/bin" HOME="$out/home" ZELL_HOME="$out/home/.zell" TMPDIR="${TMPDIR:-/tmp}" "$tc/bin/zell" "$@"; }
zell doctor || bad "zell doctor"

run_one() { # target exe expected
    case "$1" in
        linux-amd64|linux-amd64-musl) [ "$host_cpu" = x86_64 ] && echo "" || return 1 ;;
        linux-arm64|linux-arm64-musl)
            [ "$host_cpu" = aarch64 ] && { echo ""; return 0; }
            [ "$mode" = foreign ] || return 1
            if [ "$1" = linux-arm64 ]; then echo "/usr/bin/qemu-aarch64-static -L /usr/aarch64-linux-gnu"; else echo /usr/bin/qemu-aarch64-static; fi ;;
        windows-amd64) [ "$mode" = foreign ] && echo "/usr/bin/env WINEDEBUG=-all LC_ALL=C.UTF-8 /usr/bin/wine" || return 1 ;;
        *) return 1 ;;
    esac
}

for target in linux-amd64 linux-amd64-musl linux-arm64 linux-arm64-musl windows-amd64 macos-arm64 macos-amd64; do
    d=$out/$target
    mkdir -p "$d"
    ext=""
    [ "$target" = windows-amd64 ] && ext=.exe
    built=0; ran=0
    for ex in $examples; do
        src=$root/examples/$ex.zl
        exe=$d/$ex$ext
        if ! zell build --release --target "$target" "$src" -o "$exe" > "$d/$ex.build.log" 2>&1; then
            bad "$target $ex: build"; cat "$d/$ex.build.log"; continue
        fi
        built=$((built + 1))
        [ "$mode" = none ] && continue
        if runner=$(run_one "$target"); then
            # shellcheck disable=SC2086
            if ZELL_NO_BACKTRACE=1 $runner "$exe" > "$d/$ex.got" 2> "$d/$ex.err"; then
                if cmp -s "$d/$ex.got" "$root/examples/$ex.out"; then ran=$((ran + 1)); else bad "$target $ex: output differs"; diff "$root/examples/$ex.out" "$d/$ex.got" | head -20 || true; fi
            else
                bad "$target $ex: exit status $?"; head -20 "$d/$ex.err"
            fi
        fi
    done
    note "$target: $built built, $ran run and matched"
done
[ "$fail" = 0 ] && note "bundle smoke test passed" || note "bundle smoke test FAILED"
exit $fail
