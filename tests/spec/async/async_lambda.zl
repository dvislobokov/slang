// test: async lambdas are values returning Task<T>
// status: passing
namespace spec.async.async_lambda

import std
import std.io

async func main() {
    var double = async func(n int) int {
        await task.Delay(1)
        return n * 2
    }
    var t = double(21)
    println(await t, await double(5))
}
