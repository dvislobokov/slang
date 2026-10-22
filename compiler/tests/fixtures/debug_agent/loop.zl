namespace app

import std
import std.io

// Debug-agent fixture (compiler/tests/debug_agent.rs): conditional
// breakpoints, logpoints and function breakpoints.

func square(i int) int {
    var sq = i * i // BREAK square
    return sq
}

func main() {
    var total = 0
    for i in 0..10 {
        total += square(i) // BREAK loop
    }
    println("total", total)
}
