// test: println evaluates all its arguments before writing anything (Go's fmt.Println), so output from an argument comes first
// status: passing
namespace spec.strings2.println_evaluates_args_first

import std.io

func Noisy(s string) string {
    println("evaluating", s)
    return s
}

func main() {
    println("result:", Noisy("a"), Noisy("b"))
}
