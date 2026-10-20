// test: a weak pointer held across an await observes that another task released the object meanwhile
// status: passing
namespace spec.memory2.weak_across_async

import std.io
import std

struct Res {
    Name string
    func Drop() { println("drop", this.Name) }
}

struct Holder {
    Strong *Res
}

async func Release(h *Holder) {
    await task.Delay(5)
    h.Strong = nil
    println("released")
}

async func Watch(w weak *Res) {
    var before *Res = w
    println("before", before != nil)
    before = nil
    await task.Delay(30)
    var after *Res = w
    println("after", after == nil)
}

async func main() {
    var h = new Holder{Strong: new Res{Name: "r"}}
    var w weak *Res = h.Strong
    var t1 = Watch(w)
    var t2 = Release(h)
    await t2
    await t1
}
