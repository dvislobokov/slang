// Examples for std.maps.
namespace maps_test

import std
import std.io

func ExampleSortedKeys() {
    var ages = map[string]int{"bob": 31, "ann": 27, "cid": 45}
    println(maps.SortedKeys(ages))
    // Output:
    // [ann bob cid]
}

func ExampleSortedValues() {
    var ages = map[string]int{"bob": 31, "ann": 27, "cid": 45}
    // values in the order of their sorted keys
    println(maps.SortedValues(ages))
    // Output:
    // [27 31 45]
}

func ExampleMerge() {
    var a = map[string]int{"x": 1, "y": 2}
    var b = map[string]int{"y": 20, "z": 30}
    var m = maps.Merge(a, b)
    println(maps.SortedKeys(m), m["y"])
    // Output:
    // [x y z] 20
}

func ExampleFilter() {
    var stock = map[string]int{"apples": 0, "pears": 4, "plums": 7}
    var inStock = maps.Filter(stock, (k string, v int) => v > 0)
    println(maps.SortedKeys(inStock))
    // Output:
    // [pears plums]
}
