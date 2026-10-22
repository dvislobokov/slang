// test: Equatable admits types with Equals (== in the generic body calls it); List.Contains/IndexOf work on such elements
// status: passing
namespace spec.operators.equatable_constraint

import std.collections
import std.io

struct Key {
    Name string
    Id   int
    // equal by Id only
    func Equals(o Key) bool { return this.Id == o.Id }
}

func IndexOf<T: Equatable>(xs []T, x T) int {
    for i, y in xs {
        if y == x {
            return i
        }
    }
    return -1
}

func main() {
    var ks = [Key{Name: "a", Id: 1}, Key{Name: "b", Id: 2}]
    println(IndexOf(ks, Key{Name: "zzz", Id: 2}), IndexOf(ks, Key{Id: 3}), IndexOf([3, 4, 5], 5))
    var l = List<Key>{}
    l.Add(Key{Name: "x", Id: 7})
    println(l.Contains(Key{Id: 7}), l.Contains(Key{Id: 8}), l.IndexOf(Key{Name: "?", Id: 7}))
}
