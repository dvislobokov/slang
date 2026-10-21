// The users table through std.data (docs/FRAMEWORK.md section 3): the
// entity (with the password hash, which the API never returns: queries
// project it to models.User) and the data context. The schema comes from
// the migrations in migrations/ (written by `slang data migration add`).
namespace webapp.db

import std.data

@table("users")
struct UserRow {
    Id int
    @maxLength(40) Name string
    @index(unique: true) Email string
    Role string
    Password string
}

struct UsersDb {
    Context
    Users Set<UserRow>
}
