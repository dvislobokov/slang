// error: data.Open<Plain>: Plain is not a context
// status: passing
namespace spec.data.err_not_context

import std

struct Plain {
    Name string
}

async func main() void! {
    _ = data.Open<Plain>(data.Provider.Sqlite, ":memory:")?
}
