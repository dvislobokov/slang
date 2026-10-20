namespace app

var table = map[string]int{"one": 1, "two": 2}

var config = Config{
    Name: "app",
    Port: 8080,
}

func main() {
    var empty = []
    var m = map[string][]int{}
    var nested = Outer{In: Inner{A: 1}, List: [1, 2, 3]}
    var xs = [
        1,
        2,
        3,
    ]
    var p = new Point{X: 1, Y: 2}
    var t = (1, "two")
    var (a, b) = pair()
    a, b = b, a
}
