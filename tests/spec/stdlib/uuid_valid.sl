// test: uuid.New produces valid, distinct version-4 UUIDs; IsValid checks the textual form
// status: passing
namespace spec.stdlib.uuid_valid

import std.io
import std

func main() {
    var a = uuid.New()
    var b = uuid.New()
    println(len(a), a != b, uuid.IsValid(a), a[14..15])
    println(uuid.IsValid("123e4567-e89b-12d3-a456-426614174000"), uuid.IsValid("123e4567e89b12d3a456426614174000"), uuid.IsValid("xyz"), uuid.IsValid(""))
}
