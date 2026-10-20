// test: a missing file error matches fs.ErrNotFound through errors.Is
// status: passing
namespace spec.stdlib.io_readfile_not_found_is

import std
import std.io

func main() {
    try io.ReadFile("/nonexistent-slang-spec/missing.txt") catch err {
        println(errors.Is(err, fs.ErrNotFound))
    }
}
