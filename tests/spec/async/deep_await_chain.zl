// test: a chain of 200 nested awaits (recursive async func) completes
// status: passing
namespace spec.async.deep_await_chain

import std
import std.io

async func Depth(n int) int {
    if n == 0 {
        await task.Delay(1)
        return 0
    }
    return await Depth(n - 1) + 1
}

async func main() {
    println(await Depth(200))
}
