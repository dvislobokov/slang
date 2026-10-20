// test: async methods on structs can read and mutate this across awaits
// status: passing
namespace spec.async.async_method

import std.io
import std

struct Account {
    Balance int

    async func Deposit(n int) int {
        await task.Delay(1)
        this.Balance += n
        return this.Balance
    }
}

async func main() {
    var a = new Account{Balance: 10}
    println(await a.Deposit(5))
    println(await a.Deposit(7), a.Balance)
}
