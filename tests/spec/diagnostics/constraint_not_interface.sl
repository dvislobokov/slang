// error: is not an interface
// status: passing
namespace spec.diagnostics.constraint_not_interface

import std.io

struct P { X int }

func F<T: P>(x T) T { return x }

func main() {
    println(F(1))
}
