// test: task.Spawn runs an async function value on a worker; task.Yield reschedules the current task
// status: passing
// run-flags: --leak-check
namespace spec.async.task_spawn_yield

import std.io
import std

async func Twice(n int) int {
    await task.Yield()
    return n * 2
}

async func main() {
    var t = task.Spawn(async func() int {
        return await Twice(21)
    })
    var s = task.Spawn(async func() string {
        await task.Delay(1)
        return "spawned"
    })
    println(await t, await s)
}
