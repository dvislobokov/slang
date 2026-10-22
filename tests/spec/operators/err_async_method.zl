// error: operator methods are not async
// status: passing
namespace spec.operators.err_async_method

struct Lazy {
    N int
    async func Add(o Lazy) Lazy { return Lazy{N: this.N + o.N} }
}

func main() {
    var a = Lazy{N: 1}
    _ = a + a
}
