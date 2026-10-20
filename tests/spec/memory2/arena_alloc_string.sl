// test: a.Alloc<T>(n) slices and a.String copies live in the arena; references to heap values held by arena objects are released by Free; an arena that is not freed is freed with its last copy
// status: passing
// run-flags: --leak-check
namespace spec.memory2.arena_alloc_string

import std.io
import std

struct Tok {
    Text string
    Kids []*Tok
}

func Build(n int) int {
    var a = arena.New()
    var root = a.New<Tok>()
    root.Text = "root" + "!"
    for i in 0..n {
        var k = a.New<Tok>()
        k.Text = a.String(fmt.Format("kid{0}", i))
        root.Kids = append(root.Kids, k)
    }
    var xs = a.Alloc<int>(5)
    xs[2] = 7
    var names = a.Alloc<string>(3)
    names[0] = "x" + "y"
    println(len(root.Kids), root.Kids[2].Text, xs[2], len(xs), names[0], a.Len())
    return len(root.Kids)
}

func main() {
    println(Build(10))
    var b = arena.New()
    var c = b
    var p = c.New<Tok>()
    p.Text = "shared"
    println(p.Text, b.Len())
    b.Free()
    println(b.Len(), c.Len())
}
