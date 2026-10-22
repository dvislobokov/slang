// test: std.slices sorting is stable (Sort, SortBy, SortFunc) and BinarySearch finds positions
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.slices_sort

import std
import std.io

struct P {
    Name string
    Age  int
}

func main() {
    var xs []int = []
    var seed = 7
    for _ in 0..100 {
        seed = (seed * 1103515245 + 12345) % 2147483648
        xs = append(xs, seed % 50)
    }
    slices.Sort(xs)
    println(slices.IsSorted(xs), len(xs), xs[0], xs[99])
    var words = ["pear", "fig", "apple", "kiwi", "banana"]
    slices.Sort(words)
    println(words)
    var ps []P = []
    for i in 0..40 {
        ps = append(ps, P{Name: "p" + strings.PadLeft(fmt.Format("{0}", i), 2, "0"), Age: i % 3})
    }
    slices.SortBy(ps, (p P) => p.Age)
    var stable = true
    for i in 1..len(ps) {
        if ps[i].Age == ps[i - 1].Age && ps[i].Name < ps[i - 1].Name {
            stable = false
        }
    }
    println("stable:", stable, ps[0].Name, ps[13].Name, ps[14].Name, ps[39].Name)
    slices.SortFunc(words, (a string, b string) => len(a) < len(b))
    println(words)
    var sorted = [1, 3, 5, 7]
    var (i, ok) = slices.BinarySearch(sorted, 5)
    var (j, ok2) = slices.BinarySearch(sorted, 4)
    var (k, ok3) = slices.BinarySearch(sorted, 9)
    println(i, ok, j, ok2, k, ok3)
    var (m, ok4) = slices.BinarySearchFunc(ps, 1, (p P, age int) => p.Age - age)
    println(m, ok4)
    var empty []int = []
    slices.Sort(empty)
    println(empty, slices.IsSortedFunc(words, (a string, b string) => len(a) < len(b)))
}
