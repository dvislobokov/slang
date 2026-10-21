// Examples for std.os.
namespace os_test

import std
import std.io

func ExampleGetenv() void! {
    os.Setenv("SLANG_EXAMPLE_COLOR", "blue")?
    println(os.Getenv("SLANG_EXAMPLE_COLOR"), os.Getenv("SLANG_EXAMPLE_UNSET") == "")
    // Output:
    // blue true
}

func ExampleArgs() {
    // Args()[0] is the program; the rest are the command-line arguments.
    println(len(os.Args()) >= 1)
    // Output:
    // true
}
