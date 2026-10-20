// error: is not an interface (use an interface, Ordered or Comparable)
// status: passing
namespace spec.diagnostics.constraint_not_interface

import std.io

struct P { X int }

func F<T: P>(x T) T { return x }

func main() {
    println(F(1))
}
