namespace libb.shapes

import std

interface Shape {
    func Area() int
}

struct Square {
    Side int

    func Area() int {
        return this.Side * this.Side
    }
}

struct Rect {
    W int
    H int

    func Area() int {
        return this.W * this.H
    }
}

// Type switch: the type infos of Square/Rect must be the same in every
// object file for this to work across packages.
func Kind(s Shape) string {
    switch s {
        case Square q:
            return fmt.Format("square {0}", q.Side)
        case Rect r:
            return fmt.Format("rect {0}x{1}", r.W, r.H)
        default:
            return "unknown"
    }
}
