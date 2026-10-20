// error: range pattern needs a numeric or char subject
// status: passing
namespace spec.diagnostics.range_pattern_bad_subject

import std.io

func main() {
    var s = "b"
    switch s {
        case "a".."c": println("early")
        default: println("late")
    }
}
