// test: an error returned by an async func is propagated with await f()? and caught by try await
// status: passing
namespace spec.errors.async_error_awaited

import std.io
import std

async func Load(id int) string! {
    await task.Delay(1)
    if id < 0 {
        return error(fmt.Format("bad id {0}", id))
    }
    return fmt.Format("item{0}", id)
}

async func Pair(a, b int) string! {
    var x = await Load(a)?
    var y = await Load(b)?("second")
    return x + "+" + y
}

async func main() {
    println(try await Pair(1, 2) else "?")
    try await Pair(3, -4) catch err {
        println("error:", err)
    }
    try await Pair(-1, 2) catch err {
        println("error:", err)
    }
}
