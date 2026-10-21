// test: an object holding a closure that captures the object itself is a cycle; --leak-check reports it (exit 66)
// status: passing
// run-flags: --leak-check
// exit: 66
// stderr: leak:
namespace spec.memory2.closure_cycle_leak

import std.io

struct Button {
    Label   string
    OnClick func()
}

func main() {
    var b = new Button{Label: "ok"}
    b.OnClick = func() { println("clicked", b.Label) }
    b.OnClick()
}
