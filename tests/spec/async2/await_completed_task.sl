// test: awaiting a task that has already completed returns its value at once, and may be done repeatedly
// status: passing
namespace spec.async2.await_completed_task

import std.io
import std

async func Quick() int {
    return 42
}

async func main() {
    var t = Quick()
    await task.Delay(10)
    println(await t)
    println(await t)
    var u = Quick()
    var (a, b) = await task.All(u, Quick())
    println(a + b)
}
