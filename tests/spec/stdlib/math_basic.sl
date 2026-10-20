// test: Sqrt, Pow, Floor, Ceil, Abs, Min, Max and the Pi / E constants
// status: passing
namespace spec.stdlib.math_basic

import std.io
import std

func main() {
    println(math.Sqrt(16), math.Sqrt(2), math.Pow(2, 10), math.Pow(2, -1))
    println(math.Floor(-2.5), math.Ceil(-2.5), math.Floor(2.5), math.Ceil(2.1))
    println(math.Abs(-3.5), math.Min(1, 2), math.Max(-1, -2))
    println(math.Pi, math.E)
    println(math.IsNaN(math.Sqrt(-1)))
}
