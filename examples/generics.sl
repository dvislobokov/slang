namespace examples.generics

import std.io
import std
import std.collections

// A generic function with the builtin Ordered constraint.
func Max<T: Ordered>(a, b T) T {
    if a > b {
        return a
    }
    return b
}

// Several type parameters, function-typed parameters, inference from them.
func Apply<T, U>(xs []T, f func(T) U) []U {
    var out []U = []
    for x in xs {
        out = append(out, f(x))
    }
    return out
}

func Reduce<T, A>(xs []T, init A, f func(A, T) A) A {
    var acc = init
    for x in xs {
        acc = f(acc, x)
    }
    return acc
}

// An interface as a constraint: methods of T are callable.
interface Shape {
    func Area() float
}

struct Rect {
    W float
    H float
    func Area() float { return this.W * this.H }
}

struct Square {
    S float
    func Area() float { return this.S * this.S }
}

func TotalArea<T: Shape>(xs []T) float {
    var t = 0.0
    for x in xs {
        t += x.Area()
    }
    return t
}

// A generic struct; its methods are instantiated with it.
struct Pair<A, B> {
    First A
    Second B

    func Swap() Pair<B, A> {
        return Pair<B, A>{First: this.Second, Second: this.First}
    }
}

// A generic enum.
enum Option<T> {
    Some(value T)
    None
}

func Lookup(names []string, want string) Option<int> {
    for i, n in names {
        if n == want {
            return Option<int>.Some(i)
        }
    }
    return Option<int>.None
}

func Describe(o Option<int>) string {
    switch o {
        case Option.Some(i):
            return fmt.Format("at {0}", i)
        default:
            return "absent"
    }
}

// A generic error type.
struct NotFound<K> {
    Key K
    func Message() string {
        return fmt.Format("{0} not found", this.Key)
    }
}

func Get<K: Comparable>(m Map<K, string>, k K) string! {
    if !m.Has(k) {
        return NotFound<K>{Key: k}
    }
    return m.Get(k)
}

// A generic method on a plain struct.
struct Logger {
    Prefix string
    func Line<T>(x T) string {
        return fmt.Format("{0}{1}", this.Prefix, x)
    }
}

func main() {
    // Inference and explicit type arguments; untyped constants follow T.
    println(Max(3, 7), Max(2.5, 1.5), Max("apple", "pear"), Max<float>(1, 2) / 4)
    var b byte = 200
    println(Max(b, 100), Max<int8>(-3, -4))

    var words = ["go", "slang", "c"]
    var lens = Apply(words, (s string) => len(s))
    println(lens, Reduce(lens, 0, (a int, n int) => a + n))
    println(Apply(lens, (n int) => n > 1))

    println(TotalArea([Rect{W: 2, H: 3}, Rect{W: 1, H: 1}]), TotalArea([Square{S: 3}]))

    var p = Pair<string, int>{First: "age", Second: 42}
    var q = p.Swap()
    println(p.First, p.Second, q.First, q.Second)

    // An instantiated generic function is a value.
    var maxInt = Max<int>
    var fs []func(int, int) int = [maxInt, Max<int>]
    println(fs[0](1, 9), fs[1](8, 2))

    println(Describe(Lookup(words, "c")), Describe(Lookup(words, "x")))

    var l = Logger{Prefix: "> "}
    println(l.Line(5), l.Line<bool>(true), l.Line("text"))

    // std.collections: List, Map (insertion order), Set, Queue, Stack.
    var list = List<string>{}
    for w in words {
        list.Add(w)
    }
    list.Insert(1, "rust")
    list.Remove(0)
    println(list.Len(), list.Get(0), list.Contains("c"), list.IndexOf("slang"), list.Items())
    for i, w in list {
        println(i, w)
    }

    var ages = Map<string, int>{}
    ages.Set("bob", 31)
    ages.Set("ann", 29)
    ages.Set("cid", 40)
    ages.Set("bob", 32)
    ages.Remove("ann")
    for name, age in ages {
        println(name, age)
    }
    println(ages.Keys(), ages.Values(), ages.Get("zed"), ages.Has("cid"))

    var seen = Set<int>{}
    for n in [3, 1, 3, 2, 1] {
        seen.Add(n)
    }
    println(seen.Len(), seen.Has(2), seen.Items())

    var names = Map<int, string>{}
    names.Set(1, "one")
    println(try Get(names, 1) else "?")
    try Get(names, 7) catch err {
        var nf NotFound<int>
        println(err, errors.As(err, &nf), nf.Key)
    }

    var queue = Queue<string>{}
    var stack = Stack<string>{}
    for w in words {
        queue.Push(w)
        stack.Push(w)
    }
    println(try queue.Pop() else "", try stack.Pop() else "", queue.Len(), stack.Len())
    var empty = Stack<int>{}
    println(try empty.Pop() else -1)
}
