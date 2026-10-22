namespace app

import std
import std.collections
import std.io

// Word statistics with std.slices / std.maps / std.cmp and the newer
// std.collections types (Heap, OrderedMap, LRU, Deque).

struct WordCount {
    Word  string
    Count int
}

func main() {
    var text = "the quick brown fox jumps over the lazy dog the fox barks and the dog runs"
    var words = strings.Fields(text)
    var counts = map[string]int{}
    for w in words {
        counts[w] += 1
    }
    // map -> slice of structs, sorted by count (desc) then word: SortFunc is stable
    var stats = slices.Map(
        maps.SortedKeys(counts),
        (w string) => WordCount{Word: w, Count: counts[w]},
    )
    slices.SortFunc(stats, (a WordCount, b WordCount) => a.Count > b.Count)
    for s in stats[..4] {
        println(strings.PadRight(s.Word, 6, " "), s.Count)
    }
    println(
        "distinct:",
        len(slices.Unique(words)),
        "longest:",
        slices.MaxFunc(words, (a string, b string) => len(a) < len(b)),
    )
    println(
        "lengths:",
        slices.Map(
            slices.Chunk(words, 4),
            (c []string) => slices.Sum(slices.Map(c, (w string) => len(w))),
        ),
    )
    var (i, found) = slices.BinarySearch(maps.SortedKeys(counts), "fox")
    println("fox at", i, found, "| clamp:", cmp.Clamp(150, 0, 100), cmp.Compare("a", "b"))

    // the three most frequent words with a min-heap of size 3
    var heap = Heap<int>{}
    for s in stats {
        heap.Push(s.Count)
        if heap.Len() > 3 {
            _ = heap.Pop()
        }
    }
    println("top-3 counts (heap order):", heap.Items())

    // first-letter index, kept sorted
    var index = OrderedMap<string, []string>{}
    for w in maps.SortedKeys(counts) {
        var k = w[..1]
        index.Set(k, append(index.Get(k), w))
    }
    var parts []string = []
    for k in index.Keys() {
        parts = append(parts, k + "=" + strings.Join(index.Get(k), ","))
    }
    println(strings.Join(parts, " "))

    // a tiny LRU cache in front of an "expensive" function
    var cache = LRU<string, int>(2)
    var misses = 0
    for w in ["fox", "dog", "fox", "cat", "dog"] {
        var (v, ok) = cache.Get(w)
        if !ok {
            misses += 1
            v = len(w) * 10
            _ = cache.Set(w, v)
        }
        print(w, ":", v, " ")
    }
    println("misses:", misses, "order:", cache.Keys())

    var d = Deque<string>{}
    for w in words[..5] {
        d.PushFront(w)
    }
    println("reversed:", d.Items(), "back:", try d.PopBack() else "")
}
