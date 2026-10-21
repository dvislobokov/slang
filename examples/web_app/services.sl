// Services: the SQLite-backed user store (singleton), the token issuer
// (singleton, built by its static New) and a per-request audit trail
// (scoped: dropped with the request).
namespace webapp.services

import std
import std.database
import std.io

import webapp.models

struct Database {
    Db *sql.DB
}

struct UserStore {
    db *sql.DB

    static func New(d *Database) *UserStore {
        return new UserStore{db: d.Db}
    }

    async func Migrate() void! {
        _ = await this.db.Exec(
            "create table users (id integer primary key, name text not null, email text not null unique, role text not null, password text not null)",
            [],
        )?
    }

    async func All(role string) []User! {
        if role == "" {
            return await this.db.Query<User>(
                "select id, name, email, role from users order by id",
                [],
            )?
        }
        return await this.db.Query<User>(
            "select id, name, email, role from users where role = ? order by id",
            [role],
        )?
    }

    async func Find(id int) *User! {
        var users = await this.db.Query<User>(
            "select id, name, email, role from users where id = ?",
            [id],
        )?
        if len(users) == 0 {
            return nil
        }
        var u = users[0]
        return &u
    }

    async func Create(c CreateUser) User! {
        var role = c.Role
        if role == "" {
            role = "user"
        }
        var r = try await this.db.Exec(
            "insert into users (name, email, role, password) values (?, ?, ?, ?)",
            [c.Name, c.Email, role, crypto.hash.Sha256Hex(c.Password)],
        ) catch err {
            switch err {
                case sqlite.Error e:
                    if e.IsConstraint() {
                        return web.Error(409, "a user with e-mail " + c.Email + " already exists")
                    }
            }
            return err
        }
        return User{Id: r.LastInsertId, Name: c.Name, Email: c.Email, Role: role}
    }

    async func Delete(id int) bool! {
        var r = await this.db.Exec("delete from users where id = ?", [id])?
        return r.RowsAffected > 0
    }

    async func CheckPassword(email string, password string) *User! {
        var users = await this.db.Query<User>(
            "select id, name, email, role from users where email = ? and password = ?",
            [email, crypto.hash.Sha256Hex(password)],
        )?
        if len(users) == 0 {
            return nil
        }
        var u = users[0]
        return &u
    }
}

struct TokenService {
    key      web.auth.jwt.Key
    issuer   string
    lifetime int

    static func New(s *Settings) *TokenService {
        return new TokenService{
            key: web.auth.jwt.HS256(s.Jwt.Secret),
            issuer: s.Jwt.Issuer,
            lifetime: s.Jwt.LifetimeSeconds,
        }
    }

    func Key() web.auth.jwt.Key {
        return this.key
    }

    func Issue(u User, now int) string! {
        var c = web.auth.jwt.Claims{
            Subject: fmt.Format("{0}", u.Id),
            Name: u.Name,
            Email: u.Email,
            Roles: [u.Role],
            Issuer: this.issuer,
            IssuedAt: now,
            ExpiresAt: now + this.lifetime,
        }
        return web.auth.jwt.Sign(c, this.key)?
    }
}

// One per request: collects what the request did; printed when the
// request scope ends (Drop).
struct Audit {
    entries []string

    func Add(what string) {
        this.entries = append(this.entries, what)
    }

    func Drop() {
        if len(this.entries) > 0 {
            println("  audit:", strings.Join(this.entries, "; "))
        }
    }
}
