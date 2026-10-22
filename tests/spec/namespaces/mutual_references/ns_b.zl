// test: two namespaces of one program may refer to each other (no import cycles restriction)
// status: passing
namespace spec.ns.mutual.b

import spec.ns.mutual.a

func Pong(n int) string {
    if n == 0 {
        return "pong"
    }
    return Ping(n - 1)
}
