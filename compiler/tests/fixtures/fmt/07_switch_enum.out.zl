namespace app

enum Color { Red, Green, Blue }

enum Shape {
    Circle(r float)
    Rect(w, h float)

    func Area() float {
        switch this {
            case Shape.Circle(r): return 3.14 * r * r
            case Shape.Rect(w, h):
                return w * h
        }
        return 0
    }
}

func describe(n int) string {
    switch n {
        default:
            return "many"
        case 0: // none at all
            return "zero"
        case 1, 2:
            return "few"
        case 3..10:
            return "some"
    }
}
