// test: std.regex.Compile errors name the problem, the byte offset and the pattern; backreferences and lookaround are rejected (RE2 syntax); Pattern of a bad dynamic pattern panics
// status: passing
// exit: 2
// stderr: panic: regex: missing closing ]
namespace spec.stdlib3.regex_errors

import std
import std.io

func main() {
    var bad = ["(abc", "a)", "[a-", "x**", "+x", "x{2,1}", `(a)\1`, "(?=x)", "(?<!x)", `\p{Klingon}`, "(?z)", `\k`, `a\`]
    for p in bad {
        try regex.Compile(p) catch err {
            println(err.Message())
        }
    }
    var ok = try regex.Compile(`(?<word>\w+)`) else regex.Pattern("x")
    println(ok.String())
    var p = "[unclosed"
    var r = regex.Pattern(p)
    println(r.String())
}
