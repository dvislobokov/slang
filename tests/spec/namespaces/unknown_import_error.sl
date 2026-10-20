// error: unknown namespace
// status: passing
namespace spec.namespaces.unknown_import_error

import std.io
import std.doesnotexist

func main() {
    println("x")
}
