// test: @cfg(os: "windows") excludes a declaration on linux, so two platform variants of one function can coexist
// status: passing
namespace spec.platform.cfg_windows_excluded

import std.io

@cfg(os: "windows")
func ConfigDir() string {
    return "C:\\ProgramData"
}

@cfg(os: "linux")
func ConfigDir() string {
    return "/etc"
}

func main() {
    println(ConfigDir())
}
