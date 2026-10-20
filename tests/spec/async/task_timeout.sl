// test: task.Timeout(t, ms) yields false when t is still running after ms, true once it is done
// status: passing
// run-flags: --leak-check
namespace spec.async.task_timeout

import std.io
import std

async func Sleep(ms int) int {
    await task.Delay(ms)
    return ms
}

async func main() {
    var slow = Sleep(150)
    println(await task.Timeout(slow, 1))
    println(await task.Timeout(slow, 5000), await slow)
    var fast = Sleep(0)
    println(await task.Timeout(fast, 1000))
}
