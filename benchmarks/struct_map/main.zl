namespace bench.structmap

import std
import std.io

struct Point {
    X int
    Y int
}

func main() {
    var n = 100000
    var keys []string = []
    for i in 0..n {
        keys = append(keys, fmt.Format("point-{0}", i))
    }
    var m = map[string]Point{}
    for r in 0..20 {
        for i in 0..n {
            var k = keys[(i * 7919 + r) % n]
            var p = m[k]
            m[k] = Point{X: p.X + i, Y: p.Y + r}
        }
    }
    var sx = 0
    var sy = 0
    for i in 0..n {
        var p = m[keys[i]]
        sx += p.X
        sy += p.Y
    }
    println(len(m), sx, sy)
}
