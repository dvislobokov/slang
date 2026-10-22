// test: if / else if / else chains, nested ifs and boolean conditions
// status: passing
namespace spec.control.if_else_chains

import std.io

func Classify(n int) string {
    if n < 0 {
        return "negative"
    } else if n == 0 {
        return "zero"
    } else if n < 10 {
        if n % 2 == 0 {
            return "small even"
        }
        return "small odd"
    } else {
        return "large"
    }
}

func main() {
    for n in [-5, 0, 3, 4, 42] {
        println(n, Classify(n))
    }
}
