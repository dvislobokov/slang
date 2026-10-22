// test: std.data model: attributes (@table, @key, @column, @index, @maxLength, @required, @default, @concurrencyToken, @ignore, relations), conventions (Id keys, snake_case plural tables and columns, <Entity>Id foreign keys, FK indexes) and Configure's fluent calls, as DDL for SQLite and PostgreSQL
// status: passing
// run-flags: --leak-check
namespace spec.data.model_conventions

import std
import std.data
import std.io

enum Status { Active, Banned }

struct Customer {
    Id        int
    @column("mail") @index(unique: true) Email string
    @maxLength(80) FullName string
    Nickname  *string
    @required Note *string
    @default("now()") CreatedAt time.Time
    State     Status
    Balance   math.big.Decimal
    Settings  encoding.json.Value
    Photo     []byte
    Active    bool
    Score     float
    @concurrencyToken Version int
    @ignore   Scratch string
    Orders    []*OrderLine
}

struct OrderLine {
    Id         int
    CustomerId int
    Product    string
    Qty        int32
    Customer   *Customer
    Tags       []*Tag
}

@table("labels")
struct Tag {
    @key(generated: false) Code string
    Title string
}

struct Category {
    Id       int
    ParentId *int
    Name     string
    Parent   *Category
}

struct ShopContext {
    data.Context
    Customers  data.Set<Customer>
    Lines      data.Set<OrderLine>
    Tags       data.Set<Tag>
    Categories data.Set<Category>

    func Configure(m *data.ModelBuilder) {
        m.Entity<OrderLine>().ToTable("order_lines").HasIndex((l) => (l.Product, l.Qty)).HasName("ix_product_qty")
        m.Entity<OrderLine>().Property((l) => l.Product).HasMaxLength(200).HasColumnName("product_name")
        m.Entity<OrderLine>().HasManyToMany((l) => l.Tags).UsingTable("line_tags")
        m.Entity<Category>().Property((c) => c.Name).HasDefaultSql("'misc'")
    }
}

func show(ctx *ShopContext) {
    for s in ctx.CreateScript() {
        println(s + ";")
    }
}

async func main() void! {
    var lite = data.Open<ShopContext>(data.Provider.Sqlite, ":memory:")?
    var m = lite.Customers.Model()
    println(m.Entity, m.Table, len(m.Columns), m.Columns[m.Key].Name, m.Columns[m.Token].Name)
    for c in m.Columns {
        println(" ", c.Field, "->", c.Name, c.Type, c.Nullable, c.Key, c.Auto)
    }
    println(lite.Lines.TableName(), lite.Tags.TableName(), lite.Categories.TableName())
    println("-- sqlite")
    show(lite)
    await lite.EnsureCreated()?
    println("-- postgres")
    // no connection is made: the DDL is rendered for the provider
    show(data.OpenDB<ShopContext>(data.Provider.Postgres, lite.DB()))
}
