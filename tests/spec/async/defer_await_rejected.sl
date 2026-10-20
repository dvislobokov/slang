// error: defer requires a function call
// status: passing
namespace spec.async.defer_await_rejected

import std.io
import std

async func Cleanup() int {
    return 1
}

async func main() {
    defer await Cleanup()
    println("body")
}
