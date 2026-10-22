// test: with no imports at all, everything is reachable by its full path
// status: passing
namespace spec.namespaces.no_imports_full_paths

struct Box {
    V int
}

func main() {
    var b = spec.namespaces.no_imports_full_paths.Box{V: 2}
    std.io.println(std.fmt.Format("{0}", b.V), std.math.Sqrt(9), std.strings.Repeat("ab", 2))
}
