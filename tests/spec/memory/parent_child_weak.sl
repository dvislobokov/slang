// test: parent <-> child cycle broken with a weak back reference; both are freed
// status: passing
namespace spec.memory.parent_child_weak

import std.io

struct Child {
    Name string
    Parent weak *Parent
    func Drop() {
        println("free child", this.Name)
    }
}

struct Parent {
    Name string
    Kids []*Child
    func Drop() {
        println("free parent", this.Name)
    }
}

func Build() {
    var p = new Parent{Name: "root"}
    p.Kids = append(p.Kids, new Child{Name: "c1", Parent: p})
    p.Kids = append(p.Kids, new Child{Name: "c2", Parent: p})
    var k = p.Kids[1]
    var back *Parent = k.Parent
    println(k.Name, "has parent", back.Name)
}

func main() {
    Build()
    println("after Build")
}
