// test: task.Any returns the result of the first task to complete
// status: pending
namespace spec.async.task_any

import std.io
import std

async func After(name string, ms int) string {
    await task.Delay(ms)
    return name
}

async func main() {
    var first = await task.Any(After("slow", 300), After("fast", 1))
    println(first)
}
