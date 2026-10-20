// error: fmt.Format needs a template string
// status: passing
namespace spec.diagnostics.format_needs_template

import std.io
import std

func main() {
    println(fmt.Format())
}
