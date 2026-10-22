// test: std.cmp Compare/Less/Min/Max/Clamp/Or generic over Ordered (NaN sorts first)
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.cmp_funcs

import std
import std.io

func main() {
    println(cmp.Compare(1, 2), cmp.Compare("b", "a"), cmp.Compare(2.5, 2.5), cmp.Compare('a', 'b'))
    var nan = math.NaN()
    println(
        cmp.Compare(nan, 1.0),
        cmp.Compare(1.0, nan),
        cmp.Compare(nan, nan),
        cmp.Less(nan, -math.Inf(1)),
    )
    println(cmp.Less(1, 2), cmp.Less("z", "a"))
    println(cmp.Min(3, 7), cmp.Max(3, 7), cmp.Min("pear", "apple"), cmp.Max(1.5, -2.0))
    println(
        cmp.Clamp(15, 0, 10),
        cmp.Clamp(-1, 0, 10),
        cmp.Clamp(5, 0, 10),
        cmp.Clamp(0.5, 0.0, 1.0),
    )
    println(cmp.Or("", "default"), cmp.Or("set", "default"), cmp.Or(0, 42))
}
