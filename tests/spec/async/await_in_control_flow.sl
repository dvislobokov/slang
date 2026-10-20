// test: await inside switch cases, for loops, if conditions and try expressions
// status: passing
namespace spec.async.await_in_control_flow

import std
import std.io

async func Val(n int) int {
    await task.Delay(1)
    return n
}

async func Check(n int) int! {
    await task.Delay(1)
    if n < 0 {
        return error("negative")
    }
    return n
}

async func main() {
    var total = 0
    for i in 0..4 {
        switch await Val(i) {
            case 0:
                println("zero")
            case 2:
                total += await Val(100)
            default:
                total += await Val(i)
        }
    }
    if await Val(1) == 1 {
        println("if ok")
    }
    var x = try await Check(-1) else await Val(7)
    println(total, x)
}
