// error: MethodInfo.Call needs the receiver as its first argument
// status: passing
namespace spec.diagnostics.method_call_needs_receiver

import std.io
import std

struct P {
    func Hi() string { return "hi" }
}

func main() {
    var m = reflect.TypeOf<P>().Method("Hi")
    var r = try m.Call() else nil
    println(r == nil)
}
