// test: os.Arch is the target architecture, one of the tier 1 values
// status: passing
namespace spec.platform.os_arch

import std
import std.io

func main() {
    switch os.Arch {
        case "amd64", "arm64": println("tier 1")
        default:
            println("other", os.Arch)
    }
}
