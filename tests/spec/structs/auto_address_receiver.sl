// test: calling a mutating method on an addressable value (variable, field, slice element) takes its address
// status: passing
namespace spec.structs.auto_address_receiver

import std.io

struct Counter {
    N int

    func Inc() {
        this.N += 1
    }
}

struct Holder {
    C Counter
}

func main() {
    var c = Counter{}
    c.Inc()
    c.Inc()
    var h = Holder{}
    h.C.Inc()
    var list = [Counter{}, Counter{N: 10}]
    list[1].Inc()
    var p = &h
    p.C.Inc()
    println(c.N, h.C.N, list[0].N, list[1].N)
}
