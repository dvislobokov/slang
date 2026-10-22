// error: nested too deeply
// status: passing
namespace spec.generics2.instantiation_depth_error

import std.io

func Grow<T>(x T, n int) int {
    if n == 0 {
        return 0
    }
    return 1 + Grow([x], n - 1)
}

func main() {
    println(Grow(1, 3))
}
