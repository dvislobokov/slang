namespace app.report

// No imports at all: every name is written with its full path.
func Summary(users []app.models.User) {
    std.io.println("users:", len(users), "of", app.models.MaxUsers)
    for u in users {
        std.io.println(" -", u.ID(), u.Display())
    }
}
