// test: returning a Drop value moves it to the caller; it is dropped once, by the caller
// status: pending
namespace spec.memory.drop_move_on_return

import std.io

struct Token {
    Id int
    func Drop() {
        println("drop token", this.Id)
    }
}

func Make(id int) Token {
    var t = Token{Id: id}
    println("made", id)
    return t
}

func main() {
    var t = Make(7)
    println("caller has", t.Id)
}
