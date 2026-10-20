// test: generic async functions: async func Fetch<T>(...) T! awaited with ? and try
// status: passing
namespace spec.generics2.generic_async

import std.io
import std

async func Fetch<T>(v T, fail bool) T! {
    await task.Delay(1)
    if fail {
        return error("fetch failed")
    }
    return v
}

async func Both<A, B>(a A, b B) string! {
    var x = await Fetch(a, false)?
    var y = await Fetch(b, false)?
    return fmt.Format("{0}+{1}", x, y)
}

async func main() {
    var n = try await Fetch(5, false) else 0
    var s = try await Fetch("str", true) else "default"
    println(n, s)
    var both = try await Both(1.5, true) catch err {
        println(err)
        return
    }
    println(both)
}
