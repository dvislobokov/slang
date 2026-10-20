// error: await requires a Task
// status: passing
namespace spec.async.await_non_task_error

import std.io

async func main() {
    var x = await 5
    println(x)
}
