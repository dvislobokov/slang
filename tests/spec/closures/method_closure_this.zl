// test: a closure created in a method captures this and sees later mutations
// status: passing
namespace spec.closures.method_closure_this

import std.io

struct Acc {
    Total int

    func Adder() func(int) {
        return func(n int) {
            this.Total += n
        }
    }
}

func main() {
    var a = new Acc{}
    var add = a.Adder()
    add(5)
    add(7)
    println(a.Total)
}
