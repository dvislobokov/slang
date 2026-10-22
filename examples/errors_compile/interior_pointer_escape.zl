// error: cannot keep interior pointer variable `p` elsewhere (an interior pointer kept in a local is returned)
namespace app

struct Point {
    X int
    Y int
}

struct Line {
    From Point
    To   Point
}

func origin(l *Line) *Point {
    var p = &l.From // fine: p pins l
    p.X = 1
    return p // but it cannot leave the function
}

func main() {
    var l = new Line{}
    _ = origin(l)
}
