// error: must be handled
// status: passing
namespace spec.errors.unhandled_in_argument_error

import std.io

func F() int! {
    return 1
}

func Show(n int) {
    println(n)
}

func main() {
    Show(F())
}
