// test: for m in re.Iter(s): regex.Iter yields *Match values and ends at nil (Next() *T)
// status: passing
namespace spec.iteration.regex_iter

import std
import std.io

func main() {
    var re = regex.Pattern("[0-9]+")
    for m in re.Iter("a1 b22 c333") {
        print(m.Text, " ")
    }
    println()
    for i, m in re.Iter("x7y8") {
        print(i, m.Text, " ")
    }
    println()
}
