// test: --leak-check reports nothing for heap objects holding slices, maps and strings once they are released
// status: passing
// run-flags: --leak-check
namespace spec.memory2.new_with_slices_no_leak

import std.io

struct Doc {
    Title string
    Lines []string
    Tags map[string]int
    Kids []*Doc
}

func Build(n int) *Doc {
    var d = new Doc{Title: "doc", Lines: [], Tags: map[string]int{}}
    for i in 0..n {
        d.Lines = append(d.Lines, "line")
        d.Tags["t"] += i
        d.Kids = append(d.Kids, new Doc{Title: "kid", Lines: ["x", "y"]})
    }
    return d
}

func main() {
    var total = 0
    for _ in 0..3 {
        var d = Build(10)
        total += len(d.Lines) + len(d.Kids) + d.Tags["t"]
    }
    println("total", total)
}
