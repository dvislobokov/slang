// test: an async lambda captures variables by reference; tasks started from it see and update them
// status: passing
namespace spec.async2.async_closure_capture

import std
import std.io

async func main() {
    var total = 0
    var prefix = "n="
    var add = async func(n int) string {
        await task.Delay(n)
        total += n
        return prefix + fmt.Format("{0}", n)
    }
    var a = add(30)
    var b = add(1)
    println(await a, await b)
    prefix = "m="
    println(await add(10), total)
}
