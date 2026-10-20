// error: task.Any needs at least one task
// status: passing
namespace spec.diagnostics.task_any_empty

import std.io
import std

async func main() {
    var x = await task.Any()
    println(x)
}
