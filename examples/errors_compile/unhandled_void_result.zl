// error: result of type void! must be handled
namespace examples.bad

import std.io

func Save(name string) void! {
    if name == "" {
        return error("no name")
    }
}

func main() {
    Save("report")
    println("saved")
}
