// error: is private to namespace std.collections
// status: passing
namespace spec.diagnostics.collection_private_member

import std.io
import std.collections

func main() {
    var l = List<int>{}
    println(len(l.items))
}
