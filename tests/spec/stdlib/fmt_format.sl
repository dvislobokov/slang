// test: fmt.Format placeholders: reorder, repeat, all printable types, other text verbatim
// status: passing
namespace spec.stdlib.fmt_format

import std
import std.io

enum Color { Red, Green }

func main() {
    println(fmt.Format("{1} {0} {1}", "a", "b"))
    println(fmt.Format("int {0}, float {1}, bool {2}, char {3}", -3, 2.50, true, 'x'))
    println(fmt.Format("slice {0}, enum {1}, err {2}", [1, 2], Color.Green, error("bad")))
    println(fmt.Format("no placeholders, $1 and 100%"))
    println(fmt.Format("{0}{0}{0}", 7), fmt.Format("big {0} small {1}", 1e21, 1e-7))
    println(fmt.Format("{10}", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, "ten"))
}
