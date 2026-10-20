// error: unknown identifier WindowsOnly
// status: pending
namespace spec.platform.cfg_excluded_use_error

import std.io

@cfg(os: "windows")
func WindowsOnly() string {
    return "win"
}

@cfg(os: "linux")
func LinuxOnly() string {
    return "linux"
}

func main() {
    println(LinuxOnly(), WindowsOnly())
}
