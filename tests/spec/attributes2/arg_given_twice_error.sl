// error: given twice for attribute @note
// status: passing
namespace spec.attributes2.arg_given_twice_error

import std.io

@attribute
struct note {
    text string
}

@note("a", text: "b")
struct Thing {
    X int
}

func main() {
    println(Thing{X: 1}.X)
}
