namespace app

import std.io
import std.fmt as f
import app.models

func main() {
    // Short names through `import app.models`.
    var alice = User.New(1, "Alice", "alice@example.com")
    var bob = User.New(2, "Bob", "bob@example.com")
    bob.Rename("Robert")

    // The full path always works, imported or not.
    var carol = app.models.User.New(3, "Carol", "carol@example.com")

    var users []User = [alice, bob, carol]
    println("max users:", MaxUsers)
    println(f.Format("first user: {0} (id {1})", alice.Name, alice.ID()))

    // app.report is not imported: call it by its full path.
    app.report.Summary(users)
}
