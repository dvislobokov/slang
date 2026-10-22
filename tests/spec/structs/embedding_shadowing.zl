// test: an outer field or method shadows a promoted one of the same name
// status: passing
namespace spec.structs.embedding_shadowing

import std.io

struct Animal {
    Name string

    func Sound() string {
        return "..."
    }

    func Intro() string {
        return this.Name + " says " + this.Sound()
    }
}

struct Dog {
    Animal
    Name string

    func Sound() string {
        return "woof"
    }
}

func main() {
    var d = Dog{Animal: Animal{Name: "animal"}, Name: "rex"}
    println(d.Name, d.Animal.Name)
    println(d.Sound(), d.Animal.Sound())
    println(d.Intro())
}
