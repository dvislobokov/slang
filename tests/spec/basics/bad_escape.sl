// error: unknown escape sequence
// status: passing
namespace spec.basics.bad_escape

import std.io

func main() {
    println("bad \q escape")
}
