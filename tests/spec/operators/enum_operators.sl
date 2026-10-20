// test: enums with operator methods: the receiver is the value itself; Add and Neg on a direction, Compare for ordering
// status: passing
namespace spec.operators.enum_operators

import std.io

enum Dir {
    North
    East
    South
    West

    func index() int {
        switch this {
            case Dir.North: return 0
            case Dir.East: return 1
            case Dir.South: return 2
            case Dir.West: return 3
        }
    }
    // turning: North + East = East, East + East = South, ...
    func Add(o Dir) Dir { return of((this.index() + o.index()) % 4) }
    func Neg() Dir { return of((4 - this.index()) % 4) }
    func Compare(o Dir) int { return this.index() - o.index() }
    func Name() string {
        var names = ["N", "E", "S", "W"]
        return names[this.index()]
    }
}

func of(i int) Dir {
    var all = [Dir.North, Dir.East, Dir.South, Dir.West]
    return all[i]
}

func main() {
    var d = Dir.North
    d += Dir.East
    println(d.Name(), (d + Dir.East).Name(), (Dir.West + Dir.South).Name(), (-Dir.East).Name())
    println(Dir.North < Dir.West, Dir.South >= Dir.East, d == Dir.East)
}
