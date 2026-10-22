// Examples for std.strconv.
namespace strconv_test

import std
import std.io

func ExampleAtoi() {
    var n = try strconv.Atoi("123") else 0
    try strconv.Atoi("12a") catch err {
        println(errors.Is(err, strconv.ErrSyntax))
    }
    println(n + 1)
    // Output:
    // true
    // 124
}

func ExampleItoa() {
    println(strconv.Itoa(-45) + "!", strconv.FormatInt(255, 16), strconv.FormatInt(5, 2))
    // Output:
    // -45! ff 101
}

func ExampleQuote() {
    println(strconv.Quote("say \"hi\"\n"))
    // Output:
    // "say \"hi\"\n"
}
