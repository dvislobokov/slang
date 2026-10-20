// error: has no fields; write it without parentheses
// status: passing
namespace spec.diagnostics.variant_without_fields_called

import std.io

enum Color { Red, Green }

func main() {
    println(Color.Red())
}
