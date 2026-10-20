// error: cannot keep an interior pointer (a weak reference needs a whole object)
namespace app

struct Node {
    value int
}

struct Pair {
    first Node
    back weak *Node
}

func main() {
    var p = new Pair{}
    p.back = &p.first
}
