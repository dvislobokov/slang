// error: cannot keep an interior pointer (address of a field stored in a variable)
namespace app

struct Point {
    X int
    Y int
}

struct Line {
    From Point
    To Point
}

func main() {
    var l = new Line{}
    var p = &l.From
    p.X = 1
}
