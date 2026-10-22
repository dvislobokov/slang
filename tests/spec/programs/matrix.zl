// test: generic matrix library: Matrix<T: Ordered> with static constructors, fallible ops, generic free functions taking func(T, T) T
// status: passing
namespace spec.programs.matrix

import std
import std.io

struct Matrix<T: Ordered> {
    Rows int
    Cols int
    Data []T

    static func New(r, c int) Matrix<T> {
        var data []T = []
        var zero T
        for _ in 0..r * c {
            data = append(data, zero)
        }
        return Matrix<T>{Rows: r, Cols: c, Data: data}
    }

    static func From(r, c int, xs []T) Matrix<T>! {
        if len(xs) != r * c {
            return error(fmt.Format("need {0} values, got {1}", r * c, len(xs)))
        }
        return Matrix<T>{Rows: r, Cols: c, Data: xs}
    }

    func At(i, j int) T { return this.Data[i * this.Cols + j] }

    func Put(i, j int, v T) { this.Data[i * this.Cols + j] = v }

    func Dims() string { return fmt.Format("{0}x{1}", this.Rows, this.Cols) }

    func Add(o Matrix<T>) Matrix<T>! {
        if this.Rows != o.Rows || this.Cols != o.Cols {
            return error("dimension mismatch: " + this.Dims() + " vs " + o.Dims())
        }
        var out = Matrix<T>.New(this.Rows, this.Cols)
        for i, v in this.Data {
            out.Data[i] = v + o.Data[i]
        }
        return out
    }

    func Transpose() Matrix<T> {
        var out = Matrix<T>.New(this.Cols, this.Rows)
        for i in 0..this.Rows {
            for j in 0..this.Cols {
                out.Put(j, i, this.At(i, j))
            }
        }
        return out
    }

    func Max() T {
        var m = this.Data[0]
        for v in this.Data {
            if v > m { m = v }
        }
        return m
    }

    func Min() T {
        var m = this.Data[0]
        for v in this.Data {
            if v < m { m = v }
        }
        return m
    }

    func Print(name string) {
        println(name, this.Dims())
        for i in 0..this.Rows {
            println(" ", this.Data[i * this.Cols..(i + 1) * this.Cols])
        }
    }
}

func Mul<T: Ordered>(a, b Matrix<T>, times func(T, T) T) Matrix<T>! {
    if a.Cols != b.Rows {
        return error("cannot multiply " + a.Dims() + " by " + b.Dims())
    }
    var out = Matrix<T>.New(a.Rows, b.Cols)
    for i in 0..a.Rows {
        for j in 0..b.Cols {
            var s T
            for k in 0..a.Cols {
                s = s + times(a.At(i, k), b.At(k, j))
            }
            out.Put(i, j, s)
        }
    }
    return out
}

func MapM<T: Ordered, U: Ordered>(m Matrix<T>, f func(T) U) Matrix<U> {
    var out = Matrix<U>.New(m.Rows, m.Cols)
    for i, v in m.Data {
        out.Data[i] = f(v)
    }
    return out
}

func Identity<T: Ordered>(n int, one T) Matrix<T> {
    var m = Matrix<T>.New(n, n)
    for i in 0..n {
        m.Put(i, i, one)
    }
    return m
}

func Equal<T: Ordered>(a, b Matrix<T>) bool {
    if a.Rows != b.Rows || a.Cols != b.Cols { return false }
    for i, v in a.Data {
        if v != b.Data[i] { return false }
    }
    return true
}

func main() {
    var a = try Matrix<int>.From(2, 3, [1, 2, 3, 4, 5, 6]) else Matrix<int>.New(0, 0)
    a.Print("a")
    var t = a.Transpose()
    t.Print("t")
    var times = (x int, y int) => x * y
    var p = try Mul(a, t, times) catch err {
        println("unexpected:", err)
        return
    }
    p.Print("a*t")
    println("max", p.Max(), "min", p.Min())
    try a.Add(t) catch err {
        println("error:", err)
    }
    try Mul(a, a, times) catch err {
        println("error:", err)
    }
    var twice = try a.Add(a) else a
    twice.Print("a+a")
    var id = try Mul(a, Identity(3, 1), times) else t
    println("a*I == a", Equal(id, a), Equal(a, twice))
    var halves = MapM(a, (x int) => float(x) / 2.0)
    halves.Print("a/2")
    var f = try Matrix<float>.From(2, 2, [0.5, 1.5, -2.0, 4.25]) else Matrix<float>.New(1, 1)
    f.Transpose().Print("f^T")
    println("max", f.Max(), "min", f.Min())
    var s = try Matrix<string>.From(2, 2, ["b", "a", "d", "c"]) else Matrix<string>.New(1, 1)
    var ss = try s.Add(s) else s
    ss.Print("s+s")
    println("max", s.Max(), "min", s.Min())
    try Matrix<int>.From(2, 2, [1, 2, 3]) catch err {
        println("error:", err)
    }
}
