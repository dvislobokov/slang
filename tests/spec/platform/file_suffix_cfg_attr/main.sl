// test: @cfg before the namespace line excludes a whole file
// status: passing
namespace spec.platform.file_suffix_cfg_attr

import std.io

func main() {
    println(Backend())
}
