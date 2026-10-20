// test: a missing file error matches fs.ErrNotFound through errors.Is
// status: pending
namespace spec.stdlib.io_readfile_not_found_is

import std.io
import std

func main() {
    try io.ReadFile("/nonexistent-slang-spec/missing.txt") catch err {
        println(errors.Is(err, fs.ErrNotFound))
    }
}
