// test: a switch over a sealed interface may leave implementations to a default case
// status: pending
namespace spec.interfaces2.sealed_with_default_ok

import std.io

sealed interface Token {
    func Text() string
}

struct Num {
    V int
    func Text() string { return "num" }
}

struct Plus {
    func Text() string { return "+" }
}

struct Minus {
    func Text() string { return "-" }
}

func Classify(t Token) string {
    switch t {
        case Num n: return "number"
        default: return "operator " + t.Text()
    }
}

func main() {
    var ts []Token = [Num{V: 1}, Plus{}, Minus{}]
    for t in ts {
        println(Classify(t))
    }
}
