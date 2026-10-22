// test: capturing a weak *T in the closure breaks the cycle; --leak-check is clean and Drop runs
// status: passing
// run-flags: --leak-check
namespace spec.memory2.closure_cycle_weak_no_leak

import std.io

struct Button {
    Label   string
    OnClick func()
    func Drop() { println("drop", this.Label) }
}

func main() {
    var b = new Button{Label: "ok"}
    var self weak *Button = b
    b.OnClick = func() {
        var me *Button = self
        if me != nil {
            println("clicked", me.Label)
        }
    }
    b.OnClick()
    b = nil
    println("end")
}
