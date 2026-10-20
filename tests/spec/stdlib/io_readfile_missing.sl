// test: io.ReadFile on a missing file fails with an error that names the path
// status: passing
namespace spec.stdlib.io_readfile_missing

import std
import std.io

func main() {
    var text = try io.ReadFile("/nonexistent-slang-spec/missing.txt") catch err {
        println("failed:", strings.Contains(err.Message(), "/nonexistent-slang-spec/missing.txt"))
        return
    }
    println("unexpected", text)
}
