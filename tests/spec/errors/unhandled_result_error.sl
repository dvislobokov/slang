// error: result of type int! must be handled
// status: passing
namespace spec.errors.unhandled_result_error

import std.io

func F() int! {
    return 1
}

func main() {
    var x = F()
    println(x)
}
