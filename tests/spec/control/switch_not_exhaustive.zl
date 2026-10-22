// test: a switch over an enum need not cover every variant; unmatched does nothing
// status: passing
namespace spec.control.switch_not_exhaustive

import std.io

enum Dir { North, East, South, West }

func main() {
    for d in [Dir.North, Dir.East, Dir.South, Dir.West] {
        switch d {
            case Dir.North:
                println("up")
            case Dir.South:
                println("down")
        }
    }
    println("end")
}
