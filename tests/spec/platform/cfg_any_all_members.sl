// test: @cfg with any/all/arch, on struct fields and methods, and os constants in a switch
// status: passing
namespace spec.platform.cfg_any_all_members

import std
import std.io

struct Paths {
    @cfg(os: "windows")
    Drive string
    Root  string

    @cfg(os: "windows")
    func Sep() string { return "\\" }
    @cfg(not(os: "windows"))
    func Sep() string { return os.PathSeparator }
}

@cfg(any(arch: "amd64", arch: "arm64"))
const Bits = 64
@cfg(all(not(arch: "amd64"), not(arch: "arm64")))
const Bits = 32

@cfg(all(os: "linux", feature: "never"))
func Never() string { return missing.Thing() }

func main() {
    var p = Paths{Root: "/"}
    println(p.Root + "usr" + p.Sep() + "lib", Bits)
    switch os.Name {
        case "linux", "macos": println("unix")
        case "windows": println("windows")
        default:
            println("other")
    }
}
