// error: unknown identifier inner
// status: passing
namespace spec.errors.block_try_scope_error

import std
import std.io

func main() {
    try {
        var inner = strings.ToInt("1")?
        println(inner)
    } catch err {
        println(err)
    }
    println(inner)
}
