namespace app

// A user record.
struct User {
    Name  string // display name
    Email string // contact
    id    int

    @json(name: "created_at") CreatedAt int
    @json(skip: true)         cache     string
    @deprecated("use Name")
    Login string
    X, Y  float
    Embedded

    func Hello() string { return "hi " + this.Name }
}

struct Empty {}

interface Shape {
    func Area() float
    Named
    async func Load(id int) string!
}
