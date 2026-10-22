// error: task.Any arguments must be tasks
// status: passing
namespace spec.diagnostics.task_any_not_task

import std
import std.io

async func main() {
    var x = await task.Any(1, 2)
    println(x)
}
