// test: @cfg(not(os: "linux")) excludes a declaration on linux; @cfg(not(os: "windows")) keeps it
// status: pending
namespace spec.platform.cfg_not

import std.io

@cfg(not(os: "linux"))
func Kind() string {
    return "not linux"
}

@cfg(not(os: "windows"))
func Kind() string {
    return "unix-like"
}

func main() {
    println(Kind())
}
