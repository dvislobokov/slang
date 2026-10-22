// test: std.maps helpers (Keys, Values, SortedKeys, Merge, Invert, Filter, MapValues, Equal, Clone)
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.maps_funcs

import std
import std.io

func main() {
    var m = map[string]int{"b": 2, "a": 1, "c": 3}
    var ks = maps.Keys(m)
    slices.Sort(ks)
    var vs = maps.Values(m)
    slices.Sort(vs)
    println(ks, vs, maps.SortedKeys(m), maps.SortedValues(m))
    var merged = maps.Merge(m, map[string]int{"c": 30, "d": 4})
    println(maps.SortedKeys(merged), merged["c"], merged["d"], m["c"])
    var inv = maps.Invert(map[string]int{"x": 1, "y": 2, "z": 1})
    println(inv[1], inv[2])
    var all = maps.InvertAll(map[string]int{"x": 1, "y": 2, "z": 1})
    println(all[1], all[2])
    var big = maps.Filter(m, (k string, v int) => v >= 2)
    println(maps.SortedKeys(big))
    var strs = maps.MapValues(m, (v int) => fmt.Format("<{0}>", v))
    println(strs["a"], strs["c"])
    var c = maps.Clone(m)
    println(maps.Equal(c, m))
    c["a"] = 100
    println(maps.Equal(c, m), m["a"])
    println(maps.Equal(map[string]int{"a": 0}, map[string]int{"b": 0}))
    println(maps.EqualFunc(m, strs, (v int, s string) => fmt.Format("<{0}>", v) == s))
    maps.DeleteFunc(c, (k string, v int) => v > 50)
    println(maps.SortedKeys(c))
    var dst = map[string]int{"q": 9}
    maps.Copy(dst, m)
    println(maps.SortedKeys(dst))
}
