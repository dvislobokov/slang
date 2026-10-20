// test: an error returned from async main is printed to stderr with its trace, exit code 1
// status: passing
// exit: 1
// stderr: created at   tests/spec/errors2/async_main_error_trace.sl:12  Fetch
namespace spec.errors2.async_main_error_trace

import std.io
import std

async func Fetch() int! {
    await task.Delay(1)
    return error("timeout")
}

async func main() void! {
    var n = await Fetch()?
    println(n)
}
