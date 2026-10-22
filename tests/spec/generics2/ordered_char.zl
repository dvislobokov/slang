// test: char satisfies Ordered: Max and a generic sort work on []char
// status: passing
namespace spec.generics2.ordered_char

import std.io

func Max<T: Ordered>(a, b T) T {
    if a > b { return a }
    return b
}

func Sort<T: Ordered>(xs []T) []T {
    for i in 1..len(xs) {
        var j = i
        for j > 0 && xs[j - 1] > xs[j] {
            var t = xs[j]
            xs[j] = xs[j - 1]
            xs[j - 1] = t
            j -= 1
        }
    }
    return xs
}

func main() {
    println(Max('a', 'z'), Max('я', 'а'))
    var cs []char = ['d', 'a', 'c', 'b']
    println(Sort(cs))
    println(Sort(["pear", "fig", "apple"]), Sort([3.5, -1.0, 2.0]))
}
