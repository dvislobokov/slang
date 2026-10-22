#!/bin/sh
# Compares hello-world binary sizes: C, C++, Go and Zell (debug, --release,
# --release --static), plus Windows (windows-amd64, MinGW ABI) builds of C and
# Zell when the mingw-w64 cross runtime exists (make -C runtime
# CROSS=x86_64-w64-mingw32), a fully static musl build (linux-amd64-musl:
# make -C runtime CROSS=x86_64-linux-musl, musl-tools) and linux-arm64
# (make -C runtime CROSS=aarch64-linux-gnu, gcc-aarch64-linux-gnu).
# Usage: ./scripts/bench-size.sh   (needs clang, clang++, go; zell is optional:
#        $ZELL, else target/release/zell, else zell on PATH)
set -e
dir=$(mktemp -d)
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$dir"

cat > hello.c <<'C'
#include <stdio.h>
int main(void){ puts("Hello, world"); return 0; }
C
cat > hello.cpp <<'CPP'
#include <iostream>
int main(){ std::cout << "Hello, world" << std::endl; }
CPP
mkdir gohello
cat > gohello/main.go <<'GO'
package main
import "fmt"
func main(){ fmt.Println("Hello, world") }
GO
printf 'module hello\ngo 1.22\n' > gohello/go.mod

clang -O2 hello.c -o c_dyn
clang -O2 -static hello.c -o c_static && strip c_static
clang++ -O2 hello.cpp -o cpp_dyn
clang++ -O2 -static hello.cpp -o cpp_static && strip cpp_static
if command -v go >/dev/null 2>&1; then
    (cd gohello && go build -ldflags="-s -w" -o ../go_stripped .)
fi

ZELL=${ZELL:-$root/target/release/zell}
[ -x "$ZELL" ] || ZELL=$(command -v zell || true)
if [ -n "$ZELL" ]; then
    hello="$root/examples/hello.zl"
    "$ZELL" build "$hello" -o zell_debug || true
    "$ZELL" build --release "$hello" -o zell_release || true
    "$ZELL" build --release --static "$hello" -o zell_static || true
    if [ -f "$root/runtime/build/x86_64-pc-windows-gnu/libzell_rt.a" ]; then
        "$ZELL" build --release --target windows-amd64 "$hello" -o zell_win_release.exe || true
        clang -target x86_64-w64-mingw32 -O2 -s hello.c -o c_win.exe 2>/dev/null || true
    fi
    if [ -f "$root/runtime/build/x86_64-unknown-linux-musl/libzell_rt.a" ] && command -v musl-gcc >/dev/null 2>&1; then
        "$ZELL" build --release --target linux-amd64-musl "$hello" -o zell_musl_static || true
        musl-gcc -O2 -static -s hello.c -o c_musl_static 2>/dev/null || true
    fi
    if [ -f "$root/runtime/build/aarch64-unknown-linux-gnu/libzell_rt.a" ]; then
        "$ZELL" build --release --target linux-arm64 "$hello" -o zell_arm64_release || true
    fi
fi

printf '%-22s %10s\n' binary size_kb
for f in c_dyn c_static cpp_dyn cpp_static go_stripped zell_debug zell_release zell_static c_win.exe zell_win_release.exe \
         c_musl_static zell_musl_static zell_arm64_release; do
    [ -f "$f" ] && printf '%-22s %10d\n' "$f" "$(( $(stat -c %s "$f") / 1024 ))"
done
rm -rf "$dir"
