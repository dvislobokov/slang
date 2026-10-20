// test: enums may declare methods like structs; this is the enum value
// status: pending
namespace spec.enums.enum_methods

import std.io

enum Color {
    Red
    Green
    Blue

    func IsWarm() bool {
        return this == Color.Red
    }

    func Next() Color {
        switch this {
            case Color.Red:
                return Color.Green
            case Color.Green:
                return Color.Blue
        }
        return Color.Red
    }
}

func main() {
    var c = Color.Red
    println(c.IsWarm(), c.Next(), c.Next().Next().Next(), Color.Blue.IsWarm())
}
