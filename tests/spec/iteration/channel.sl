// test: for v in ch receives from a task.Channel until it is closed and drained (the loop awaits Next, so it is inside an async function)
// status: passing
namespace spec.iteration.channel

import std.io
import std

async func produce(ch task.Channel<int>, n int) {
    for i in 0..n {
        await ch.Send(i * i)
    }
    ch.Close()
}

async func main() void! {
    var ch = task.Channel<int>(2)
    var t = produce(ch, 5)
    var sum = 0
    for v in ch {
        print(v, " ")
        sum += v
    }
    println()
    println("sum", sum)
    await t
}
