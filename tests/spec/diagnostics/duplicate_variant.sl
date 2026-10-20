// error: duplicate variant Red in enum
// status: passing
namespace spec.diagnostics.duplicate_variant

import std.io

enum Color { Red, Green, Red }

func main() {
    println(Color.Green)
}
