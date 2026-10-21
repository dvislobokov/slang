// test: with SLANG_CYCLES=auto (the default) cycles created in a loop are collected on the way: the live object count stays bounded and --leak-check passes
// status: passing
// run-flags: --leak-check
namespace spec.memory3.auto_collect

import std
import std.io

@allowCycle
struct Session {
    Id   int
    Peer *Session
}

func churn(i int) {
    var a = new Session{Id: i}
    var b = new Session{Id: i + 1, Peer: a}
    a.Peer = b
}

func main() {
    var most = 0
    for i in 0..100000 {
        churn(i)
        if i % 1000 == 0 {
            var live = diag.Runtime().LiveObjects
            if live > most {
                most = live
            }
        }
    }
    var st = runtime.CycleStats()
    println("mode", st.Mode, "bounded", most < 100000, "collected some", st.Collected > 0, "runs", st.Runs > 0)
}
