// error: expected a condition (bool), found int
// status: passing
namespace spec.data.err_not_bool

import std

struct User {
    Id    int
    Name  string
    Age   int
    @data.ignore Cache string
    Orders []*Order
}

struct Order {
    Id     int
    UserId int
    Total  float
}

struct Shop {
    data.Context
    Users  data.Set<User>
    Orders data.Set<Order>
}

async func main() void! {
    var ctx = data.Open<Shop>(data.Provider.Sqlite, ":memory:")?
    _ = await ctx.Users.Where((u) => u.Age + 1).ToList()?
}
