// test: an attribute restricted to Variant (and Param) targets is accepted on enum variants and parameters
// status: passing
namespace spec.attributes2.targets_variant_ok

import std
import std.io

@attribute(targets: [reflect.Target.Variant, reflect.Target.Param])
struct label {
    text string
}

enum Status {
    @label("in progress") Active
    @label("finished") Done
}

func Show(@label("the status") s Status) {
    println(s)
}

func main() {
    Show(Status.Done)
}
