// error: await outside async
namespace examples.bad

import std.io

async func Compute() int {
    return 42
}

func main() {
    var n = await Compute()
    println(n)
}
