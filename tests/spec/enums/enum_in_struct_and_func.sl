// test: enums as struct fields, parameters and return values
// status: passing
namespace spec.enums.enum_in_struct_and_func

import std.io

enum Op { Plus, Minus }

struct Step {
    O Op
    N int
}

func Apply(acc int, s Step) int {
    switch s.O {
        case Op.Plus:
            return acc + s.N
        default:
            return acc - s.N
    }
}

func Flip(o Op) Op {
    if o == Op.Plus {
        return Op.Minus
    }
    return Op.Plus
}

func main() {
    var steps = [Step{O: Op.Plus, N: 10}, Step{O: Op.Minus, N: 3}, Step{O: Flip(Op.Minus), N: 1}]
    var acc = 0
    for s in steps {
        acc = Apply(acc, s)
    }
    println(acc, Flip(Op.Plus))
}
