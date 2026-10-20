// test: trigonometric, exponential and logarithm functions at exact points
// status: passing
namespace spec.stdlib.math_trig

import std.io
import std

func main() {
    println(math.Sin(0), math.Cos(0), math.Tan(0))
    println(math.Atan2(1, 1) * 4 == math.Pi, math.Atan2(0, -1))
    println(math.Log(math.E), math.Exp(0), math.Log(1))
}
