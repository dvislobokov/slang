// test: an early return drops the values created so far (last declared first); values not yet declared are not dropped
// status: passing
namespace spec.memory2.drop_early_return

import std.io

struct R {
    Name string
    func Drop() { println("drop", this.Name) }
}

func Work(stop int) int {
    var a = R{Name: "a"}
    if stop == 1 {
        return 1
    }
    var b = R{Name: "b"}
    {
        var c = R{Name: "c"}
        if stop == 2 {
            return 2
        }
    }
    var d = R{Name: "d"}
    return 3
}

func main() {
    for s in 1..4 {
        var r = Work(s)
        println("result", r)
    }
}
