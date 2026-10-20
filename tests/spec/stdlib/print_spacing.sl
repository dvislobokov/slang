// test: print(a, b) adds a space only between operands when neither is a string (Go fmt.Print)
// status: pending
namespace spec.stdlib.print_spacing

import std.io

func main() {
    print(1, 2, "\n")
    print("a", "b", 3, "\n")
    print(1, "x", 2, 3, "\n")
}
