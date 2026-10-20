// test: lowercase fields are accessible anywhere inside their own namespace, even across files
// status: passing
namespace spec.structs.privok

struct Account {
    balance int
}

func Peek(a Account) int {
    return a.balance * 10
}
