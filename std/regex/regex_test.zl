// Examples for std.regex.
namespace regex_test

import std
import std.io

func ExamplePattern() {
    // Pattern literals are checked by the compiler.
    var re = regex.Pattern(`^[a-z]+\d*$`)
    println(re.Match("abc123"), re.Match("ABC"))
    // Output:
    // true false
}

func ExampleRegex_FindAllText() {
    var words = regex.Pattern(`\b\w{4}\b`)
    println(words.FindAllText("the slow gray fox runs fast"))
    // Output:
    // [slow gray runs fast]
}

func ExampleRegex_Replace() {
    var kv = regex.Pattern(`(\w+)=(\w+)`)
    println(kv.Replace("a=1 b=2", "$2<-$1"))
    // Output:
    // 1<-a 2<-b
}

func ExampleRegex_Groups() {
    var date = regex.Pattern(`(?P<y>\d{4})-(?P<m>\d\d)-(?P<d>\d\d)`)
    var g = date.Groups("due 2024-07-15")
    println(g.Get("y"), g.Get("m"), g.Get("d"))
    // Output:
    // 2024 07 15
}

func ExampleCompile() {
    // Compile is for patterns built at run time.
    try regex.Compile("(unclosed") catch err {
        println("error:", err)
    }
    // Output:
    // error: regex: missing closing ) at offset 0 in "(unclosed"
}
