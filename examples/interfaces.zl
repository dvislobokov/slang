namespace examples.interfaces

import std
import std.io

interface Shape {
    func Area() float
    func Name() string
}

interface Named {
    func Name() string
}

struct Circle {
    R float

    func Area() float {
        return 3.14159 * this.R * this.R
    }

    func Name() string {
        return "circle"
    }
}

struct Rect {
    W float
    H float

    func Area() float {
        return this.W * this.H
    }

    func Name() string {
        return "rect"
    }
}

struct Square {
    Side float

    func Area() float {
        return this.Side * this.Side
    }

    func Name() string {
        return "square"
    }
}

func Describe(s Shape) {
    println(s.Name(), "area", s.Area())
}

func TotalArea(shapes []Shape) float {
    var total = 0.0
    for s in shapes {
        total += s.Area()
    }
    return total
}

// Type switch: each case binds the concrete value.
func Detail(s Shape) string {
    switch s {
        case Circle c:
            return fmt.Format("circle with radius {0}", c.R)
        case Rect r:
            return fmt.Format("rect {0}x{1}", r.W, r.H)
        default:
            return "some other shape: " + s.Name()
    }
}

func main() {
    var c = Circle{R: 2}
    var r = Rect{W: 3, H: 4.5}
    Describe(c)
    Describe(r)

    var shapes []Shape = [c, r, Square{Side: 2}, Circle{R: 1}]
    for i, s in shapes {
        println(i, Detail(s))
    }
    println("total area:", TotalArea(shapes))

    // A value satisfies every interface whose methods it has.
    var named Named = r
    println("named:", named.Name())

    // An interface can hold a pointer; changes through the pointer are visible.
    var big = new Circle{R: 10}
    var s Shape = big
    println("big area:", s.Area())
    big.R = 1
    println("after shrinking:", s.Area())
}
