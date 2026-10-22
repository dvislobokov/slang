// test: a Drop that stores another member of its dead cycle in a global keeps that member alive; no Drop runs twice
// status: passing
namespace spec.memory3.drop_resurrect

import std
import std.io

@allowCycle
struct Node {
    Name  string
    Other *Node

    func Drop() {
        println("drop", this.Name)
        if this.Name == "a" {
            saved = this.Other
        }
    }
}

var saved *Node

func main() {
    var a = new Node{Name: "a"}
    var b = new Node{Name: "b", Other: a}
    a.Other = b
    a = nil
    b = nil
    println("collected", runtime.CollectCycles())
    println("saved", saved.Name)
    println("collected", runtime.CollectCycles())
}
