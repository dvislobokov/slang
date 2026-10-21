// error: task.Any arguments must have the same type
// status: passing
namespace spec.diagnostics.task_any_mixed_types

import std
import std.io

async func A() int { return 1 }
async func B() string { return "b" }

async func main() {
    var x = await task.Any(A(), B())
    println(x)
}
