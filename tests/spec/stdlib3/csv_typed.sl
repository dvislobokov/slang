// test: std.encoding.csv.Read<T>/Write<T>/ReadNoHeader<T> through compile-time reflection: @csv(name:), @csv(index:), @csv(skip:), @serde(name:), ints with range checks, floats, bools, plain enums by name, *T (empty cell = nil); errors name the line and column
// status: passing
namespace spec.stdlib3.csv_typed

import std
import std.io

enum Level {
    Low
    High
}

struct Person {
    @csv(name: "full_name") Name string
    Age    int8
    Score  float
    Active bool
    Level  Level
    @csv(skip: true)     cache string
    @serde(name: "nick") Nick  *string
}

struct Point {
    @csv(index: 1) Y int
    @csv(index: 0) X int
    Label string
}

func main() void! {
    var text = "Level,full_name,Age,Score,Active,nick,extra\nHigh,Ann,31,4.5,true,annie,x\nLow,Bob,25,3,no,,y\n"
    var people = encoding.csv.Read<Person>(text)?
    for p in people {
        var nick = "nil"
        if p.Nick != nil {
            nick = *p.Nick
        }
        println(p.Name, p.Age, p.Score, p.Active, p.Level, nick, p.cache == "")
    }
    print(encoding.csv.Write(people))

    var pts = encoding.csv.ReadNoHeader<Point>("1,2,a\n3,4,\"b, c\"\n")?
    for pt in pts {
        println(pt.X, pt.Y, pt.Label)
    }
    print(encoding.csv.Write(pts))

    var bad = [
        "full_name,Age,Score,Active,Level,nick\nX,old,1,true,Low,\n",
        "full_name,Age,Score,Active,Level,nick\nX,300,1,true,Low,\n",
        "full_name,Age,Score,Active,Level,nick\nX,1,1,maybe,Low,\n",
        "full_name,Age,Score,Active,Level,nick\nX,1,1,true,Medium,\n",
        "full_name,Age,Score,Active,nick\nX,1,1,true,\n",
        "",
        "full_name,Age\n\"x,1\n",
    ]
    for b in bad {
        try encoding.csv.Read<Person>(b) catch err {
            println(err.Message())
        }
    }
    try encoding.csv.ReadNoHeader<Point>("1\n") catch err {
        println(err.Message())
    }
}
