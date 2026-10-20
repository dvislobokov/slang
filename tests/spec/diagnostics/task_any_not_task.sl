// error: task.Any arguments must be tasks
// status: passing
namespace spec.diagnostics.task_any_not_task

import std.io
import std

async func main() {
    var x = await task.Any(1, 2)
    println(x)
}
