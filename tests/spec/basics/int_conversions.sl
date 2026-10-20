// test: explicit conversions between integer widths truncate or sign-extend
// status: passing
namespace spec.basics.int_conversions

import std.io

func main() {
    var x = 300
    var neg = -1
    println(int8(x), uint8(x), int16(x), byte(x))
    println(uint8(neg), uint16(neg), uint32(neg), uint64(neg))
    var small int8 = -5
    println(int(small), int64(small), uint8(small))
    var u uint32 = 4000000000
    println(int32(u), int64(u))
    println(int(3.99), int(-3.99), float(7) / 2)
}
