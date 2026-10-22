namespace examples.enums

import std.io

enum Color { Red, Green, Blue }

// Variants may carry fields; plain variants can be mixed in.
enum Shape {
    Circle(radius float)
    Rect(w float, h float)
    Empty
}

enum Result {
    Ok(value int)
    Fail(code int, reason string)
}

func ColorName(c Color) string {
    switch c {
        case Color.Red:
            return "red"
        case Color.Green:
            return "green"
        case Color.Blue:
            return "blue"
    }
    return "unknown"
}

func IsWarm(c Color) bool {
    switch c {
        case Color.Red:
            return true
        default:
            return false
    }
}

func Area(s Shape) float {
    switch s {
        case Shape.Circle(r):
            return 3.0 * r * r
        case Shape.Rect(w, h):
            return w * h
        default:
            return 0.0
    }
}

func Divide(a, b int) Result {
    if b == 0 {
        return Result.Fail(1, "division by zero")
    }
    return Result.Ok(a / b)
}

func Report(r Result) {
    switch r {
        case Result.Ok(v):
            println("ok:", v)
        case Result.Fail(code, reason):
            println("fail:", code, reason)
    }
}

func main() {
    var c = Color.Green
    println("color:", ColorName(c))
    println("printed directly:", c)
    println("ordinal of Blue:", int(Color.Blue))
    println("c == Color.Green:", c == Color.Green, "c == Color.Red:", c == Color.Red)
    for col in [Color.Red, Color.Green, Color.Blue] {
        println(ColorName(col), "warm:", IsWarm(col))
    }

    var shapes = [Shape.Circle(1.0), Shape.Rect(2.0, 3.5), Shape.Empty]
    for s in shapes {
        println("area:", Area(s))
    }

    Report(Divide(10, 3))
    Report(Divide(1, 0))

    var r = Divide(20, 4)
    switch r {
        case Result.Ok(v):
            if v > 3 {
                println("big result", v)
            }
        default:
            println("not ok")
    }
}
