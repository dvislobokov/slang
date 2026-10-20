#!/bin/sh
# Compares hello-world binary sizes: C, C++, Go and Slang (when `slang build` works).
# Usage: ./scripts/bench-size.sh   (needs clang, clang++, go; slang is optional)
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
(cd gohello && go build -ldflags="-s -w" -o ../go_stripped .)

if command -v slang >/dev/null 2>&1 || [ -x "$root/target/release/slang" ]; then
    SLANG=${SLANG:-$root/target/release/slang}
    "$SLANG" build "$root/examples/hello.sl" -o slang_dyn && strip slang_dyn || true
fi

printf '%-16s %10s\n' binary size_kb
for f in c_dyn c_static cpp_dyn cpp_static go_stripped slang_dyn; do
    [ -f "$f" ] && printf '%-16s %10d\n' "$f" "$(( $(stat -c %s "$f") / 1024 ))"
done
rm -rf "$dir"
