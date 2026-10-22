// test: generic slice helpers Map<T, U> and Filter<T> with lambdas
// status: passing
namespace spec.generics.map_filter

import std
import std.io

func Map<T, U>(xs []T, f func(T) U) []U {
    var out []U = []
    for x in xs {
        out = append(out, f(x))
    }
    return out
}

func Filter<T>(xs []T, keep func(T) bool) []T {
    var out []T = []
    for x in xs {
        if keep(x) {
            out = append(out, x)
        }
    }
    return out
}

func main() {
    var nums = [1, 2, 3, 4, 5, 6]
    var evens = Filter(nums, (n int) => n % 2 == 0)
    var labels = Map(evens, (n int) => fmt.Format("#{0}", n))
    println(evens, labels)
    println(Map(["a", "bb", "ccc"], (s string) => len(s)))
}
