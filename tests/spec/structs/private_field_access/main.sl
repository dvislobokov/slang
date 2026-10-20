// error: private
// status: passing
namespace spec.structs.priv

import std.io
import spec.structs.priv.models

func main() {
    var a = Account.Open(10)
    println(a.Balance())
    println(a.balance)
}
