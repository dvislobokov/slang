namespace examples.arith

import std
import std.io

const Pi = 3.14159
const Answer = 42
const Greeting = "const string"

func main() {
    // Integers: inferred, explicit type, zero value.
    var a = 17
    var b int = 5
    var c int
    println("a b c:", a, b, c)
    println("sum:", a + b)
    println("diff:", a - b)
    println("prod:", a * b)
    println("quot:", a / b)
    println("rem:", a % b)
    // Division truncates toward zero; the remainder takes the sign of the dividend.
    println("neg quot:", -a / b)
    println("neg rem:", -a % b)
    println("rem neg divisor:", a % -b)

    // Precedence (same table as Go).
    println("2 + 3 * 4 =", 2 + 3 * 4)
    println("(2 + 3) * 4 =", (2 + 3) * 4)
    println("10 - 4 - 3 =", 10 - 4 - 3)
    println("100 / 10 / 5 =", 100 / 10 / 5)
    println("1 + 2 << 3 =", 1 + 2 << 3)
    println("6 & 3, 6 | 3, 6 ^ 3:", 6 & 3, 6 | 3, 6 ^ 3)
    println("1 << 10 =", 1 << 10, "1024 >> 3 =", 1024 >> 3)

    // Compound assignment.
    var n = 10
    n += 5
    n -= 3
    n *= 2
    n /= 5
    n %= 3
    println("compound:", n)

    // Floats print with the shortest representation that round-trips.
    var x = 0.5
    var y float = 2.0
    var z float
    println("x y z:", x, y, z)
    println("x + y:", x + y)
    println("x * y:", x * y)
    println("y / 4.0:", y / 4.0)
    println("7.0 / 2.0:", 7.0 / 2.0)
    println("0.1 + 0.2:", 0.1 + 0.2)
    println("1.5e3:", 1.5e3)
    println("-x:", -x)

    var r = 2.0
    println("area:", Pi * r * r)
    println("sqrt(2):", math.Sqrt(2.0))
    println("pow:", math.Pow(2.0, 10.0))
    println("floor:", math.Floor(3.7))
    println("abs:", math.Abs(-2.5))

    // No implicit int/float mixing: convert explicitly.
    var i = 7
    println("float(i) / 2.0:", float(i) / 2.0)
    println("int(3.99):", int(3.99))
    println("int(-3.99):", int(-3.99))

    // Comparisons.
    println("a > b:", a > b, "a == 17:", a == 17, "a != 17:", a != 17)
    println("b <= 5:", b <= 5, "b >= 6:", b >= 6, "x < y:", x < y)

    // Boolean logic: && binds tighter than ||.
    var t = true
    var f = false
    println("t && f:", t && f)
    println("t || f:", t || f)
    println("!t:", !t)
    println("true || false && false:", true || false && false)
    println("(true || false) && false:", (true || false) && false)
    var inRange = a > 10 && a < 20
    println("in range:", inRange)

    // Constants.
    println("const:", Answer, Greeting)
    const Local = Answer * 2
    println("local const:", Local)
}
