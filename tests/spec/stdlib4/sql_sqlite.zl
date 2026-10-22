// test: std.database.sql over the SQLite driver (in-memory): Exec/Result, Query<T> with @sql.column, snake_case names, pointers and sql.Null<T>, time.Time, big.Decimal, []byte, bool, named parameters, QueryRow and ErrNoRows, Scan, Get<T>, transactions with defer Rollback, Transact, Prepare, Conn, driver registry, typed driver errors, conversion errors
// status: passing
// run-flags: --leak-check
namespace spec.stdlib4.sql_sqlite

import std
import std.database
import std.io

struct User {
    Id   int
    Name string
    @sql.column("mail") Email *string
    CreatedAt time.Time
    Score     sql.Null<float>
    Balance   math.big.Decimal
    Active    bool
    Avatar    []byte
    @sql.column(skip: true) Note string
}

struct Pair {
    A int
    B string
}

func show(u User) string {
    var e = "<nil>"
    if u.Email != nil {
        e = *u.Email
    }
    return fmt.Format(
        "{0} {1} {2} {3} {4} {5} {6} {7} {8}",
        u.Id,
        u.Name,
        e,
        u.CreatedAt.Format(time.RFC3339),
        u.Score.String(),
        u.Balance.String(),
        u.Active,
        len(u.Avatar),
        u.Note,
    )
}

async func main() void! {
    println("available", sqlite.Available(), strings.HasPrefix(sqlite.Version(), "3."))
    sql.Register("sqlite", sqlite.Driver{})
    println("drivers", sql.Drivers())
    try sql.Open("nope", "") catch err {
        println(err)
    }
    var db = sql.Open("sqlite", "file::memory:?mode=memory&_foreign_keys=on")?
    db.SetMaxOpenConns(1)
    defer db.Close()
    await db.Ping()?
    _ = await db.Exec(
        "create table users (id integer primary key, name text not null unique, mail text, created_at datetime, score real, balance text, active boolean, avatar blob)",
        [],
    )?
    var t0 = time.Date(2024, 2, 29, 12, 30, 0, 0, time.UTC())
    var r = await db.Exec(
        "insert into users (name, mail, created_at, score, balance, active, avatar) values (?, ?, ?, ?, ?, ?, ?)",
        [
            "ann",
            "ann@example.com",
            t0,
            9.5,
            math.big.MustParseDecimal("10.25"),
            true,
            strings.Bytes("PNG"),
        ],
    )?
    println("insert", r.RowsAffected, r.LastInsertId)
    r = await db.Exec(
        "insert into users (name, created_at, score, balance, active) values (:name, :at, :score, :bal, :on)",
        [sql.Named("name", "bob"), sql.Named("at", t0.AddDate(0, 0, 1)), sql.Named(
            "score",
            sql.Null<float>{},
        ), sql.Named("bal", "0"), sql.Named("on", false)],
    )?
    println("insert", r.RowsAffected, r.LastInsertId)
    var named = map[string]any{}
    named["n"] = "cy"
    named["s"] = sql.Some(2.0)
    _ = await db.Exec(
        "insert into users (name, created_at, balance, active, score) values (:n, '2020-01-02 03:04:05', '-1.5', 1, :s)",
        [named],
    )?
    try await db.Exec("insert into users (name) values (:n)", [named, 5]) catch err {
        println(err)
    }
    try await db.Exec("insert into users (name) values (:missing)", [named]) catch err {
        println(err)
    }

    // typed queries
    var users = await db.Query<User>("select * from users order by id", [])?
    for u in users {
        println(show(u))
    }
    var one = await db.QueryOne<User>("select * from users where name = ?", ["bob"])?
    println("one", show(one))
    try await db.QueryOne<User>("select * from users where id = ?", [99]) catch err {
        println(errors.Is(err, sql.ErrNoRows), err)
    }
    println("count", await db.QueryOne<int>("select count(*) from users", [])?)
    println("names", await db.Query<string>("select name from users order by name desc", [])?)
    var pairs = await db.Query<Pair>(
        "select id as a, name as b from users where active = ? order by id",
        [true],
    )?
    println(len(pairs), pairs[0].A, pairs[0].B, pairs[1].B)
    var nums = await db.Query<sql.Null<int>>("select null union all select 7", [])?
    println(nums[0].Valid, nums[1].Valid, nums[1].Value)

    // rows and Scan
    var rows = await db.QueryRows("select id, name, mail, score from users order by id", [])?
    println(rows.Columns(), rows.ColumnTypes())
    for await rows.Next()? {
        var id int
        var name string
        var mail *string
        var score sql.Null<float>
        rows.Scan([&id, &name, &mail, &score])?
        var label = rows.Get<string>(1)?
        println("row", id, name, mail != nil, score.String(), label)
    }
    var row = await db.QueryRow("select name, score from users where id = ?", [2])
    var nm string
    var sc float
    try row.Scan([&nm, &sc]) catch err {
        println("scan:", err)
    }
    row = await db.QueryRow("select name from users where id = ?", [42])
    try row.Scan([&nm]) catch err {
        println("scan:", err)
    }
    var anyv any
    row = await db.QueryRow("select 1.5", [])
    row.Scan([&anyv])?
    println("any", anyv)

    // driver errors are typed
    try await db.Exec("insert into users (name) values (?)", ["ann"]) catch err {
        var se sqlite.Error
        println(err, errors.As(err, &se) && se.IsConstraint(), se.ExtendedCode)
    }

    // transactions
    var tx = await db.Begin()?
    _ = await tx.Exec("update users set score = score + 1", [])?
    println("in tx", await tx.QueryOne<float>("select score from users where id = 1", [])?)
    await tx.Rollback()?
    try await tx.Commit() catch err {
        println(errors.Is(err, sql.ErrTxDone))
    }
    println("rolled back", await db.QueryOne<float>("select score from users where id = 1", [])?)
    await committed(db)?
    println("committed", await db.QueryOne<float>("select score from users where id = 1", [])?)
    try await db.Transact(async func(t sql.Tx) void! {
        _ = await t.Exec("delete from users", [])?
        return error("abort")
    }) catch err {
        println("transact:", err)
    }
    await db.Transact(async func(t sql.Tx) void! {
        _ = await t.Exec("update users set active = 0 where id = ?", [1])?
    })?
    println("still", await db.QueryOne<int>("select count(*) from users where active", [])?)

    // prepared statements and a dedicated connection
    var st = await db.Prepare("select name from users where id = :id")?
    for id in [1, 3] {
        var n = await st.QueryOne<string>([sql.Named("id", id)])?
        println("stmt", id, n)
    }
    try await db.Prepare("selec nonsense") catch err {
        println("prepare:", err)
    }
    var c = await db.Conn()?
    _ = await c.Exec("create temp table tmp (x int)", [])?
    _ = await c.Exec("insert into tmp values (1), (2)", [])?
    println("temp", await c.QueryOne<int>("select sum(x) from tmp", [])?)
    c.Close()

    // conversion errors
    try await db.QueryOne<int>("select name from users where id = 1", []) catch err {
        println(err)
    }
    try await db.QueryOne<int>("select null", []) catch err {
        println(err)
    }
    try await db.QueryOne<int8>("select 300", []) catch err {
        println(err)
    }
    var s = db.Stats()
    println("stats", s.OpenConnections, s.Idle, s.InUse)
}

async func committed(db *sql.DB) void! {
    var tx = await db.Begin()?
    defer tx.Rollback()
    _ = await tx.Exec("update users set score = 100 where id = 1", [])?
    await tx.Commit()?
}
