// error: duplicate member
// status: passing
namespace spec.diagnostics.enum_duplicate_member

import std.io

enum Color {
    Red
    func Name() string { return "a" }
    func Name() string { return "b" }
}

func main() {
    println(Color.Red.Name())
}
