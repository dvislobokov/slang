// test: async lambdas are values returning Task<T>
// status: pending
namespace spec.async.async_lambda

import std.io
import std

async func main() {
    var double = async func(n int) int {
        await task.Delay(1)
        return n * 2
    }
    var t = double(21)
    println(await t, await double(5))
}
