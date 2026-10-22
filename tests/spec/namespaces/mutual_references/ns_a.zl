// test: two namespaces of one program may refer to each other (no import cycles restriction)
// status: passing
namespace spec.ns.mutual.a

import spec.ns.mutual.b

func Ping(n int) string {
    if n == 0 {
        return "ping"
    }
    return Pong(n - 1)
}
