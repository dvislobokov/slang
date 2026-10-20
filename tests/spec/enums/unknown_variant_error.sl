// error: has no variant Purple
// status: passing
namespace spec.enums.unknown_variant_error

import std.io

enum Color { Red, Green }

func main() {
    println(Color.Purple)
}
