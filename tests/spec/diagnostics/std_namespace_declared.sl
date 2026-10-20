// error: user code cannot declare a std namespace
// status: passing
namespace std.mine

import std.io

func main() {
    println("x")
}
