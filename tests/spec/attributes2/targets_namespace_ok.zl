// test: an attribute restricted to Namespace targets is accepted before the namespace line
// status: passing
@spec.attributes2.targets_namespace_ok.module("core")
namespace spec.attributes2.targets_namespace_ok

import std
import std.io

@attribute(targets: [reflect.Target.Namespace])
struct module {
    name string
}

func main() {
    println("ok")
}
