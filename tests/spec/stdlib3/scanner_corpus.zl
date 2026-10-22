// test: std.text.scanner against Go's text/scanner (generated; Go oracle): idents incl. Unicode, numbers with prefixes/underscores/exponents, strings/chars/raw strings, comments skipped and kept, every error message and its position
// status: passing
namespace spec.stdlib3.scanner_corpus

import std

func run(src string, comments bool) {
    var s = text.scanner.New(src)
    if comments {
        s.Mode = s.Mode & (1023 - text.scanner.SkipComments)
    }
    for {
        var tok = s.Scan()
        if tok == text.scanner.EOF {
            break
        }
        io.println(
            fmt.Format(
                "  {0} {1} {2}:{3}",
                text.scanner.TokenString(tok),
                encoding.json.Quote(s.TokenText()),
                s.Pos.Line,
                s.Pos.Column,
            ),
        )
    }
    for e in s.Errors {
        io.println("  error " + e)
    }
    io.println(fmt.Format("  errors {0}", s.ErrorCount))
}
func main() {
    io.println("case 0 false")
    run("x := 42 + y_1*3.14", false)
    io.println("case 1 false")
    run("func main() { fmt.Println(\"hi\\n\", 'a', `raw\nstr`) }", false)
    io.println("case 2 false")
    run("// line comment\na /* block */ b", false)
    io.println("case 2 true")
    run("// line comment\na /* block */ b", true)
    io.println("case 3 false")
    run("0x1F 0o17 0b101 017 1_000 1e10 1.5e-3 .5 0x1p-2 0.", false)
    io.println("case 4 false")
    run("héllo wörld 日本", false)
    io.println("case 5 false")
    run("'ab' \"unterminated", false)
    io.println("case 6 false")
    run("/* unterminated", false)
    io.println("case 6 true")
    run("/* unterminated", true)
    io.println("case 7 false")
    run("0x 0b2 09 1e", false)
    io.println("case 8 false")
    run("1__0 0x_1 1_ 0b1.0 0o7e1 1p3 0x1.8", false)
    io.println("case 9 false")
    run("a.b->c <= d != e", false)
    io.println("case 10 false")
    run("'\\x41' '\\u00e9' \"\\q\"", false)
    io.println("case 11 false")
    run("", false)
    io.println("case 12 false")
    run("   \t\n  ", false)
}
