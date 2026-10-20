// test: an error returned from async main is printed to stderr and the exit code is 1
// status: passing
// exit: 1
// stderr: boom
namespace spec.async.async_main_error_exit

import std.io
import std

async func main() void! {
    await task.Delay(1)
    println("before")
    return error("boom")
}
