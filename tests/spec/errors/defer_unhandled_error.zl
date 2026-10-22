// error: must be handled
// status: passing
namespace spec.errors.defer_unhandled_error

import std.io

func Close() void! {
    return error("x")
}

func main() {
    defer Close()
    println("body")
}
