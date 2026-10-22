// Examples for std.fmt.
namespace fmt_test

import std
import std.io

func ExampleFormat() {
    // {N} is argument N, formatted as println would; arguments can repeat.
    println(fmt.Format("{0} has {1} items, {0}!", "cart", 3))
    // Output:
    // cart has 3 items, cart!
}

func ExampleSprintf() {
    println(fmt.Sprintf("%-6s|%5.2f|%03d|%x|%q", "id", 3.14159, 7, 255, "hi"))
    // Output:
    // id    | 3.14|007|ff|"hi"
}
