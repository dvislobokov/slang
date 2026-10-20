namespace app.models

// Exported (capitalised) names are visible to other namespaces;
// the lowercase field `id` is private to app.models.
struct User {
    Name string
    Email string
    id int

    func ID() int {
        return this.id
    }

    func Display() string {
        return this.Name + " <" + this.Email + ">"
    }

    func Rename(name string) {
        this.Name = name
    }

    static func New(id int, name, email string) User {
        return User{id: id, Name: name, Email: email}
    }
}

const MaxUsers = 3
