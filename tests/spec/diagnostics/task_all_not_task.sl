// error: task.All arguments must be tasks
// status: passing
namespace spec.diagnostics.task_all_not_task

import std.io
import std

async func main() {
    await task.All(1, 2)
    println("x")
}
