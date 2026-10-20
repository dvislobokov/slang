// error: help: strings is in std: add `import std`
// status: passing
namespace spec.diagnostics.help_missing_import

import std.io

func main() {
    println(strings.ToUpper("x"))
}
