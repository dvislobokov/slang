// error: panic needs a string or an error
// status: passing
namespace spec.diagnostics.panic_bad_arg

import std.io

func main() {
    panic(42)
}
