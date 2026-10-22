// error: errors.As target must be a pointer to a struct implementing error
// status: passing
namespace spec.diagnostics.errors_as_bad_target

import std
import std.io

func main() {
    var e = error("x")
    var n = 0
    println(errors.As(e, &n))
}
