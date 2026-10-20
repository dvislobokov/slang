// test: os.PathSeparator and os.LineSeparator are string constants ("/" and "\n" on linux)
// status: pending
namespace spec.platform.path_separator

import std.io
import std

const Sep = os.PathSeparator

func main() {
    println(Sep == "/", len(os.PathSeparator), os.LineSeparator == "\n")
    println(strings.Join(["usr", "local", "bin"], os.PathSeparator))
}
