// error: task.All needs at least one task
// status: passing
namespace spec.diagnostics.task_all_empty

import std.io
import std

async func main() {
    await task.All()
    println("x")
}
