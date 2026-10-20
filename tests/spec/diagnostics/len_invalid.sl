// error: invalid argument for len
// status: passing
namespace spec.diagnostics.len_invalid

import std.io

func main() {
    println(len(42))
}
