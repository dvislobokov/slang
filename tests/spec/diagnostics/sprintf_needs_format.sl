// error: fmt.Sprintf needs a format string
// status: passing
namespace spec.diagnostics.sprintf_needs_format

import std.io
import std

func main() {
    println(fmt.Sprintf())
}
