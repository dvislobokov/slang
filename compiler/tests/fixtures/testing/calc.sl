namespace calc

import std.io

// A tiny library with tests next to it (calc_test.sl), for `slang test`.

func Add(a int, b int) int {
    return a + b
}

func Div(a int, b int) int! {
    if b == 0 {
        return error("division by zero")
    }
    return a / b
}

func main() {
    println("calc main (renamed away by slang test)")
}
