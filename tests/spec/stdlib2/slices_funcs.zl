// test: std.slices transformation helpers (Map, Filter, Reduce, Unique, Chunk, Window, Zip, ...)
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.slices_funcs

import std
import std.io

func main() {
    var xs = [3, 1, 4, 1, 5, 9, 2, 6]
    println(slices.Map(xs, (x int) => x * 10))
    println(slices.Filter(xs, (x int) => x % 2 == 0))
    println(slices.Reduce(xs, "", (acc string, x int) => acc + fmt.Format("{0}", x)))
    println(
        slices.Index(xs, 5),
        slices.Index(xs, 7),
        slices.Contains(xs, 9),
        slices.IndexFunc(xs, (x int) => x > 4),
    )
    println(
        slices.ContainsFunc(xs, (x int) => x > 100),
        slices.All(xs, (x int) => x > 0),
        slices.Count(xs, (x int) => x == 1),
    )
    println(slices.Unique(xs), slices.Compact([1, 1, 2, 2, 2, 1, 3, 3]))
    println(slices.Chunk(xs, 3), slices.Window([1, 2, 3, 4], 2), slices.Window([1], 2))
    var z = slices.Zip(["a", "b", "c"], [1, 2])
    println(len(z), z[0].First, z[0].Second, z[1].First, z[1].Second)
    println(slices.Flatten([[1, 2], [], [3]]), slices.Concat([1], [2, 3]))
    println(slices.Min(xs), slices.Max(xs), slices.Sum(xs), slices.Sum([0.5, 0.25]), slices.Sum([
        "a",
        "b",
    ]))
    println(
        slices.MinFunc(["bb", "a", "ccc"], (a string, b string) => len(a) < len(b)),
        slices.MaxFunc(["bb", "a", "ccc"], (a string, b string) => len(a) < len(b)),
    )
    println(slices.Equal(xs, slices.Clone(xs)), slices.Equal([1], [2]), slices.Compare([1, 2], [
        1,
        3,
    ]), slices.Compare([1, 2], [1]), slices.Compare([1], [1]))
    println(slices.EqualFunc([1, 2], ["1", "2"], (a int, b string) => fmt.Format("{0}", a) == b))
    println(slices.Insert([1, 4], 1, [
        2,
        3,
    ]), slices.Delete([0, 1, 2, 3, 4], 1, 3), slices.DeleteFunc(xs, (x int) => x < 4))
    var r = [1, 2, 3]
    slices.Reverse(r)
    println(r)
    slices.Fill(r, 7)
    println(r, slices.Repeat([1, 2], 3), slices.Of("x", 3))
    var shuffled = [1, 2, 3, 4, 5, 6]
    var seed = 42
    slices.Shuffle(shuffled, func(n int) int {
        seed = (seed * 31 + 7) % 1000
        return seed % n
    })
    slices.Sort(shuffled)
    println(shuffled)
    var groups = slices.GroupBy(
        ["apple", "avocado", "banana", "blueberry", "cherry"],
        (s string) => s[0],
    )
    for g in groups {
        println(string(char(g.First)), g.Second)
    }
}
