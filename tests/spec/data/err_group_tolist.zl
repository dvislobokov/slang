// error: select the group values first
// status: passing
namespace spec.data.err_group_tolist

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
    _ = await ctx.Orders.GroupBy((o) => o.UserId).ToList()?
}
