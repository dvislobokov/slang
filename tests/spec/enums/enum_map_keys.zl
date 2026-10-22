// test: plain enum values can be map keys
// status: passing
namespace spec.enums.enum_map_keys

import std.io

enum Fruit { Apple, Pear, Plum }

func main() {
    var counts = map[Fruit]int{}
    for f in [Fruit.Apple, Fruit.Plum, Fruit.Apple, Fruit.Apple] {
        counts[f] += 1
    }
    println(counts[Fruit.Apple], counts[Fruit.Pear], counts[Fruit.Plum], len(counts))
    var names = map[Fruit]string{Fruit.Pear: "pear"}
    println(names[Fruit.Pear])
}
