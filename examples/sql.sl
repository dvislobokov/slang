// std.database.sql with the SQLite driver (in-memory): schema, inserts
// with positional and named parameters, typed Query<T> into structs
// (@sql.column, pointers for NULL, time.Time), QueryOne, a transaction
// with the `defer tx.Rollback()` idiom, and ErrNoRows.
namespace examples.sql

import std.io
import std
import std.database

struct Book {
    Id int
    Title string
    Author string
    @sql.column("published") Year int
    Rating *float
    AddedAt time.Time
}

async func addBook(db *sql.DB, title string, author string, year int) int! {
    var r = await db.Exec("insert into books (title, author, published, added_at) values (:t, :a, :y, :at)", [sql.Named("t", title), sql.Named("a", author), sql.Named("y", year), sql.Named("at", time.Date(2024, 1, 15, 9, 30, 0, 0, time.UTC()))])?
    return r.LastInsertId
}

async func rate(db *sql.DB, id int, stars float) void! {
    var tx = await db.Begin()?
    defer tx.Rollback()
    var n = await tx.Exec("update books set rating = ? where id = ?", [stars, id])?
    if n.RowsAffected != 1 {
        return error("no book " + fmt.Format("{0}", id))
    }
    await tx.Commit()?
}

async func main() void! {
    var db = sqlite.Open(":memory:")
    defer db.Close()
    _ = await db.Exec("create table books (id integer primary key, title text not null, author text not null, published integer, rating real, added_at datetime)", [])?
    var dune = await addBook(db, "Dune", "Frank Herbert", 1965)?
    _ = await addBook(db, "Neuromancer", "William Gibson", 1984)?
    _ = await addBook(db, "Hyperion", "Dan Simmons", 1989)?
    await rate(db, dune, 4.5)?
    try await rate(db, 99, 1) catch err {
        println("rate:", err)
    }

    var books = await db.Query<Book>("select * from books where published < ? order by published", [1988])?
    for b in books {
        var rating = "unrated"
        if b.Rating != nil {
            rating = fmt.Format("{0} stars", *b.Rating)
        }
        println(b.Id, b.Title, "by", b.Author, b.Year, rating, b.AddedAt.Format("2006-01-02"))
    }
    println("books:", await db.QueryOne<int>("select count(*) from books", [])?)
    var titles = await db.Query<string>("select title from books order by title", [])?
    println(titles)
    try await db.QueryOne<Book>("select * from books where id = ?", [42]) catch err {
        println(errors.Is(err, sql.ErrNoRows), err)
    }
}
