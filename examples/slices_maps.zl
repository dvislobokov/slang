namespace examples.collections

import std.io

func Sum(xs []int) int {
    var total = 0
    for x in xs {
        total += x
    }
    return total
}

func main() {
    // Slices.
    var nums = [5, 3, 8]
    println("nums:", nums, "len:", len(nums))
    nums = append(nums, 1)
    nums = append(nums, 9)
    println("appended:", nums, "len:", len(nums))
    println("first:", nums[0], "last:", nums[len(nums) - 1])
    nums[1] = 30
    println("after nums[1] = 30:", nums)

    // Sub-slice: half-open range, shares the underlying buffer.
    var mid = nums[1..4]
    println("nums[1..4]:", mid, "len:", len(mid))
    mid[0] = 300
    println("after mid[0] = 300:", nums)
    println("sum:", Sum(nums))

    var empty []string = []
    println("empty len:", len(empty))
    var words []string
    println("zero-value slice len:", len(words))
    words = append(words, "alpha")
    words = append(words, "beta")
    for i, w in words {
        println(i, w)
    }

    var grid = [[1, 2], [3, 4], [5, 6]]
    var total = 0
    for row in grid {
        for v in row {
            total += v
        }
    }
    println("grid:", grid, "total:", total)

    var squares []int = []
    for i in 1..6 {
        squares = append(squares, i * i)
    }
    println("squares:", squares)

    // Fixed-size array: zero-initialised.
    var arr [3]int
    arr[0] = 7
    println("array:", arr, "len:", len(arr))

    // Maps.
    var ages = map[string]int{"alice": 31, "bob": 25}
    ages["carol"] = 40
    ages["bob"] = 26
    println("len:", len(ages))
    println("bob:", ages["bob"])
    println("missing key gives zero value:", ages["zed"])
    delete(ages, "alice")
    println("after delete len:", len(ages))
    println("alice after delete:", ages["alice"])

    // Iteration order is unspecified: print through a known key list...
    var keys = ["bob", "carol"]
    for k in keys {
        println(k, "->", ages[k])
    }
    // ...and only aggregate when iterating the map itself.
    var sumAges = 0
    var entries = 0
    for _, age in ages {
        sumAges += age
        entries += 1
    }
    println("sum of ages:", sumAges, "entries:", entries)
    var keyBytes = 0
    for k in ages {
        keyBytes += len(k)
    }
    println("total key length:", keyBytes)

    // Word count with compound assignment on a map element.
    var counts = map[string]int{}
    for w in ["a", "b", "a", "c", "a", "b"] {
        counts[w] += 1
    }
    for w in ["a", "b", "c"] {
        println(w, counts[w])
    }

    // Maps have reference semantics.
    var alias = counts
    alias["d"] = 9
    println("counts[d] via alias:", counts["d"])

    // Map of slices.
    var groups = map[string][]int{}
    for n in 1..7 {
        if n % 2 == 0 {
            groups["even"] = append(groups["even"], n)
        } else {
            groups["odd"] = append(groups["odd"], n)
        }
    }
    println("even:", groups["even"], "odd:", groups["odd"])
}
