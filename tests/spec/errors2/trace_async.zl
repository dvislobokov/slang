// test: the error trace records each `await f()?` an error passes through async functions
// status: passing
namespace spec.errors2.trace_async

import std
import std.io

async func Fetch(url string) string! {
    await task.Delay(1)
    return error("unreachable: " + url)
}

async func Load() string! {
    var body = await Fetch("a")?
    return body
}

async func main() {
    try await Load() catch err {
        println(err)
        println(errors.Trace(err))
    }
}
