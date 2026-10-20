// test: std.collections LinkedList, Deque, Ring, Heap, PriorityQueue, OrderedMap, LRU and BitSet
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.collections_more

import std.collections
import std.io

struct Job {
    Name string
    Pri int
}

func main() {
    var l = LinkedList<string>{}
    l.PushBack("b")
    l.PushBack("c")
    l.PushFront("a")
    l.Insert(1, "x")
    println(l.Items(), l.Len(), l.Get(2), try l.Front() else "", try l.Back() else "")
    println(l.RemoveAt(1), try l.PopFront() else "", try l.PopBack() else "", l.Items(), l.Len())
    _ = l.PopBack()
    println(try l.PopBack() else "empty", l.Len())
    for _ in 0..1000 {
        l.PushBack("n")
    }
    l.Clear()
    println(l.Len())

    var d = Deque<int>{}
    for i in 0..10 {
        d.PushBack(i)
        d.PushFront(-i)
    }
    println(d.Len(), d.Get(0), d.Get(19), try d.PopFront() else 0, try d.PopBack() else 0, try d.Front() else 0, try d.Back() else 0)
    d.Set(0, 100)
    println(d.Items())

    var r = Ring<int>(3)
    for i in 1..6 {
        print(r.Push(i), " ")
    }
    println(r.Items(), r.Len(), r.Cap(), r.IsFull(), try r.Pop() else 0, r.Get(0))

    var h = Heap<int>{}
    for x in [5, 3, 8, 1, 9, 2] {
        h.Push(x)
    }
    var sorted []int = []
    for h.Len() > 0 {
        sorted = append(sorted, try h.Pop() else -1)
    }
    var h2 = Heap<string>.From(["pear", "apple", "fig"])
    println(sorted, try h2.Peek() else "", h2.Len(), try h.Pop() else -1)

    var pq = PriorityQueue<Job>((a Job, b Job) => a.Pri > b.Pri)
    pq.Push(Job{Name: "low", Pri: 1})
    pq.Push(Job{Name: "high", Pri: 9})
    pq.Push(Job{Name: "mid1", Pri: 5})
    pq.Push(Job{Name: "mid2", Pri: 5})
    while_pop(pq)

    var om = OrderedMap<string, int>{}
    om.Set("pear", 3)
    om.Set("apple", 1)
    om.Set("fig", 2)
    om.Set("apple", 10)
    println(om.Keys(), om.Values(), om.Get("fig"), om.Get("nope"), om.Has("pear"), om.Len())
    println(om.Remove("fig"), om.Remove("fig"), om.Keys(), try om.Min() else "", try om.Max() else "")
    println(try om.Ceiling("b") else "", try om.Floor("b") else "", try om.Floor("a") else "none", om.Range("a", "p"))

    var c = LRU<string, int>(2)
    _ = c.Set("a", 1)
    _ = c.Set("b", 2)
    var (va, oka) = c.Get("a")
    println(va, oka, c.Set("c", 3), c.Keys(), c.Has("b"), c.Len(), c.Evictions())
    var (vb, okb) = c.Get("b")
    var (vp, okp) = c.Peek("a")
    println(vb, okb, vp, okp, c.Remove("a"), c.Keys())
    _ = c.Set("c", 30)
    var (vc, _) = c.Get("c")
    println(vc)

    var bs = BitSet{}
    bs.Set(1)
    bs.Set(64)
    bs.Set(130)
    var other = BitSet{}
    other.Set(1)
    other.Set(2)
    println(bs.Items(), bs.Count(), bs.Len(), bs.Has(64), bs.Has(63), bs.Union(other).Items(), bs.Intersect(other).Items(), bs.Difference(other).Items())
    bs.Clear(64)
    bs.Toggle(3)
    println(bs.Items(), bs.Equals(bs.Union(BitSet{})), bs.Equals(other))
}

func while_pop(pq PriorityQueue<Job>) {
    var q = pq
    for q.Len() > 0 {
        var j = try q.Pop() else Job{}
        print(j.Name, " ")
    }
    println()
}
