// test: import app.models.User imports a single type by its short name
// status: passing
namespace spec.ns.typeimp.models

struct User {
    Name string

    static func New(n string) User {
        return User{Name: n}
    }
}

func Count() int {
    return 1
}
