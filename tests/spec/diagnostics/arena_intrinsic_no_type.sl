// error: needs a type argument
// status: passing
namespace spec.diagnostics.arena_intrinsic_no_type

import std
import std.io

func main() {
    var a = arena.New()
    var p = arena.rtArenaNew(1)
    println(a.Len(), p == nil)
}
