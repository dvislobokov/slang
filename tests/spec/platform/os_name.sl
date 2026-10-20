// test: os.Name is the target operating system ("linux" on the CI and development machines)
// status: pending
namespace spec.platform.os_name

import std.io
import std

func main() {
    println(os.Name)
    println(os.Name == "linux")
}
