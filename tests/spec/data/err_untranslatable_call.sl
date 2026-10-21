// error: expression cannot be translated to SQL: function isAdult cannot be translated
// status: passing
namespace spec.data.err_untranslatable_call

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

func isAdult(u *User) bool {
    return u.Age >= 18
}

async func main() void! {
    var ctx = data.Open<Shop>(data.Provider.Sqlite, ":memory:")?
    _ = await ctx.Users.Where((u) => isAdult(u)).ToList()?
}
