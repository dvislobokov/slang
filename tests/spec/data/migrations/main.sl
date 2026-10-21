// test: std.data migrations as code: @migration structs (as written by `slang data migration add`) applied by ctx.Migrate() in id order, each in a transaction, recorded in __migrations and not applied twice; the Migrator DDL for SQLite and PostgreSQL (CreateTable, AddColumn, AlterColumn, RenameColumn, CreateIndex, DropIndex, AddForeignKey, Sql); MigrationScript
// status: passing
// run-flags: --leak-check
namespace spec.data.migrations

import std
import std.io

struct Note {
    Id      int
    Title   string
    Body    string
    Starred bool
}

struct Notes {
    data.Context
    Notes data.Set<Note>
}

async func main() void! {
    var ctx = data.Open<Notes>(data.Provider.Sqlite, ":memory:")?
    for m in data.Migrations() {
        println("migration", m.Id)
    }
    await ctx.Migrate()?
    await ctx.Migrate()?
    var ids = await ctx.Raw<string>("select id from __migrations order by id", [])?
    println(ids)
    ctx.Notes.Add(new Note{Title: "hello", Body: "world", Starred: true})
    _ = await ctx.SaveChanges()?
    var n = await ctx.Notes.Where((x) => x.Starred).Select((x) => x.Title + " " + x.Body).First()?
    println(n)

    for p in [data.Provider.Sqlite, data.Provider.Postgres] {
        var m = data.NewMigrator(p)
        m.AddColumn("notes", data.Column{Name: "views", Type: "int64"})
        m.AlterColumn("notes", data.Column{Name: "title", Type: "string", MaxLength: 200})
        m.RenameColumn("notes", "body", "text")
        m.DropIndex("notes", "ix_notes_title")
        m.AddForeignKey("notes", data.ForeignKey{Columns: ["owner_id"], RefTable: "users", RefColumns: ["id"], OnDelete: "CASCADE"})
        m.SqlFor(data.Provider.Postgres, "CREATE EXTENSION IF NOT EXISTS citext")
        for s in m.Statements() {
            println(s)
        }
    }
    print(data.MigrationScript(data.Provider.Postgres, data.Migrations()))
}
