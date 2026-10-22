namespace examples.structs

import std
import std.io

struct Point {
    X float
    Y float

    func Length() float {
        return math.Sqrt(this.X * this.X + this.Y * this.Y)
    }

    func Move(dx, dy float) {
        this.X += dx
        this.Y += dy
    }

    func String() string {
        return fmt.Format("({0}, {1})", this.X, this.Y)
    }

    static func New(x, y float) Point {
        return Point{X: x, Y: y}
    }

    static func Origin() Point {
        return Point{}
    }
}

struct Counter {
    count int

    func Inc() {
        this.count += 1
    }

    func Value() int {
        return this.count
    }
}

struct Node {
    Value int
    Next  *Node
}

struct Animal {
    Name string

    func Describe() string {
        return "I am " + this.Name
    }

    func Rename(name string) {
        this.Name = name
    }
}

// Dog embeds Animal: Animal's fields and methods are promoted.
struct Dog {
    Animal
    Breed string

    func Speak() string {
        return this.Name + " says woof"
    }
}

// Structs are values: p is a copy.
func Shift(p Point) Point {
    p.X += 100
    return p
}

func ShiftInPlace(p *Point) {
    p.X += 100
}

func main() {
    var p = Point.New(3, 4)
    println("p:", p.String(), "length:", p.Length())
    p.Move(1, 1)
    println("moved:", p.String())
    var q = Point{X: 1.5, Y: -2}
    println("q:", q.String())
    var o = Point.Origin()
    println("origin:", o.String())

    // Value semantics.
    var a = Point{X: 1, Y: 2}
    var b = a
    b.X = 99
    println("a:", a.String(), "b:", b.String())
    var shifted = Shift(a)
    println("after Shift: a =", a.String(), "result =", shifted.String())

    // Pointers: &, *, automatic dereference on field access.
    var ptr = &a
    ptr.X = 10
    println("after ptr.X = 10: a =", a.String())
    (*ptr).Y = 20
    println("after (*ptr).Y = 20: a =", a.String())
    ShiftInPlace(&a)
    println("after ShiftInPlace: a =", a.String())
    var snapshot = *ptr
    snapshot.X = 0
    println("snapshot:", snapshot.String(), "a:", a.String())
    ptr.Move(1, 1)
    println("after ptr.Move: a =", a.String())

    // Methods mutate their receiver.
    var c = Counter{}
    c.Inc()
    c.Inc()
    c.Inc()
    println("counter:", c.Value())

    // new allocates on the heap and returns a pointer.
    var heap = new Point{X: 7, Y: 8}
    heap.Move(-7, -8)
    println("heap:", heap.String(), "length:", heap.Length())
    var alias = heap
    alias.X = 5
    println("heap via alias:", heap.String())

    // nil pointers.
    var list *Node
    println("empty list is nil:", list == nil)
    for i in 0..3 {
        list = new Node{Value: i, Next: list}
    }
    var total = 0
    var cur = list
    for cur != nil {
        total += cur.Value
        cur = cur.Next
    }
    println("list head:", list.Value, "sum:", total)
    println("end of list is nil:", list.Next.Next.Next == nil)

    // Embedding.
    var d = Dog{Animal: Animal{Name: "Rex"}, Breed: "beagle"}
    println(d.Describe())
    println(d.Speak())
    println("breed:", d.Breed, "name:", d.Name)
    d.Rename("Max")
    println(d.Speak())
    println("via embedded field:", d.Animal.Name)
}
