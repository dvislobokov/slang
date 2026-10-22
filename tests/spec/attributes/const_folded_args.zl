// test: attribute arguments may be constant expressions referring to consts
// status: passing
namespace spec.attributes.const_folded_args

import std.io

const Base = 10
const Name = "svc"

@attribute
struct limit {
    min   int
    max   int
    label string
    ratio float
}

@limit(min: Base * 2, max: Base * 10 + 5, label: Name + "-v1", ratio: 1.0 / 4)
func main() {
    println("ok")
}
