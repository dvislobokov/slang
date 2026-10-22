// test: task.Any returns the result of the first task to complete
// status: passing
namespace spec.async.task_any

import std
import std.io

async func After(name string, ms int) string {
    await task.Delay(ms)
    return name
}

async func main() {
    var first = await task.Any(After("slow", 300), After("fast", 1))
    println(first)
}
