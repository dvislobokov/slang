// test: < <= > >= lower to Compare(other) compared with 0; Compare also makes a type satisfy Ordered
// status: passing
namespace spec.operators.compare_method

import std.io

struct Version {
    Major int
    Minor int

    func Compare(o Version) int {
        if this.Major != o.Major {
            return this.Major - o.Major
        }
        return this.Minor - o.Minor
    }
    func Equals(o Version) bool { return this.Compare(o) == 0 }
}

func Max<T: Ordered>(a T, b T) T {
    if a > b {
        return a
    }
    return b
}

func main() {
    var a = Version{Major: 1, Minor: 2}
    var b = Version{Major: 1, Minor: 10}
    var c = Version{Major: 2, Minor: 0}
    println(a < b, a <= b, a > b, a >= b, b < c, c <= c, c > c)
    println(a == Version{Major: 1, Minor: 2}, a != b)
    var m = Max(a, b)
    println(m.Major, m.Minor)
    var n = Max(c, b)
    println(n.Major, n.Minor, Max(3, 9), Max("a", "b"))
}
