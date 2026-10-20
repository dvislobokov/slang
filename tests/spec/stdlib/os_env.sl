// test: Setenv / Getenv round trip; unset variables read as empty
// status: passing
namespace spec.stdlib.os_env

import std
import std.io

func main() {
    println("[" + os.Getenv("SLANG_SPEC_SURELY_UNSET_VAR") + "]")
    try os.Setenv("SLANG_SPEC_VAR", "v1") catch err {
        println("setenv failed", err)
    }
    println(os.Getenv("SLANG_SPEC_VAR"))
}
