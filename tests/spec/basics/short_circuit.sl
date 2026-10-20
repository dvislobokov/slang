// test: && and || short-circuit; ! binds tighter than &&, && tighter than ||
// status: passing
namespace spec.basics.short_circuit

import std.io

func Say(name string, v bool) bool {
    println("eval", name)
    return v
}

func main() {
    println(Say("a", false) && Say("b", true))
    println(Say("c", true) || Say("d", true))
    println(!false && false || true)
    println(true || false && false)
}
