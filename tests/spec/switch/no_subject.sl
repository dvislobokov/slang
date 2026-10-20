// test: switch without a subject tests boolean conditions in order
// status: passing
namespace spec.switch.no_subject

import std.io

func Grade(score int) string {
    switch {
        case score >= 90:
            return "A"
        case score >= 80 && score < 90:
            return "B"
        case score < 0:
            return "invalid"
        default:
            return "C"
    }
}

func main() {
    println(Grade(95), Grade(85), Grade(-1), Grade(50))
}
