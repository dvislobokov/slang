// error: private
// status: passing
namespace spec.structs.priv.models

struct Account {
    balance int

    func Balance() int {
        return this.balance
    }

    static func Open(n int) Account {
        return Account{balance: n}
    }
}
