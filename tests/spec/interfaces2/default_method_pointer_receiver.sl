// test: a default method called on an interface holding *T sees the current state of the object
// status: passing
namespace spec.interfaces2.default_method_pointer_receiver

import std
import std.io

interface Account {
    func Balance() int
    func Deposit(n int)
    func Report() string { return fmt.Format("balance={0}", this.Balance()) }
}

struct Wallet {
    cents int
    func Balance() int { return this.cents }
    func Deposit(n int) { this.cents += n }
}

func main() {
    var w = new Wallet{}
    var a Account = w
    println(a.Report())
    a.Deposit(25)
    w.Deposit(5)
    println(a.Report())
}
