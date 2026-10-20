// error: a for range needs both bounds
// status: passing
namespace spec.diagnostics.for_range_missing_bound

import std.io

func main() {
    for i in 0.. {
        println(i)
    }
}
