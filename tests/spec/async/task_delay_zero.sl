// test: task.Delay(0) completes and output order follows awaits only
// status: passing
namespace spec.async.task_delay_zero

import std.io
import std

async func main() {
    for i in 0..3 {
        await task.Delay(0)
        println("tick", i)
    }
    await task.Delay(2)
    println("done")
}
