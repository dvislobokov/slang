// error: declare `func Equals(other Point) bool` in Point to compare with == and !=
// status: passing
namespace spec.operators.err_equals_missing

struct Point {
    X int
}

func main() {
    var a = Point{X: 1}
    _ = a == Point{X: 1}
}
