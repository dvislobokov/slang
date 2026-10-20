// error: cannot assign to undeclared variable a
// status: passing
namespace spec.diagnostics.tuple_assign_undeclared

import std.io

func Two() (int, int) { return 1, 2 }

func main() {
    (a, b) = Two()
}
