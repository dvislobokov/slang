// error: @validate(minLength) on field Signup.Age of type int: minLength applies to strings, slices and maps
// status: passing
namespace spec.web.err_validate_kind

import std.web

struct Signup {
    @validate(required: true) Name string
    @validate(minLength: 18)  Age  int
}

func main() {
    var app = NewApp()
    app.MapPost("/signup", (s Signup) => s.Name)
}
