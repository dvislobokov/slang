// error: cannot assign to undeclared variable total
// status: passing
namespace spec.diagnostics.assign_undeclared

import std.io

func main() {
    total = 5
}
