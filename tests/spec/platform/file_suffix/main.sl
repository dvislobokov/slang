// test: files named *_linux.sl are compiled only on linux and *_windows.sl only on windows
// status: passing
namespace spec.platform.file_suffix

import std.io

func main() {
    println(PlatformName(), Separator())
}
