// error: unknown identifier Count
// status: passing
namespace spec.ns.typeonly.models

struct User {
    Name string
}

func Count() int {
    return 1
}
