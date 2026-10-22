// test: two files may declare the same namespace and see each other's declarations
// status: passing
namespace spec.ns.shared

const Step = 5

struct Counter {
    N int
    func Add(n int) { this.N += n }
}

func describe(c Counter) string {
    if c.N > 5 {
        return "big"
    }
    return "small"
}
