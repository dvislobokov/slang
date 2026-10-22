// test: switch on strings with value lists and default
// status: passing
namespace spec.control.switch_strings

import std.io

func Kind(ext string) string {
    switch ext {
        case "go", "rs", "sl":
            return "source"
        case "md":
            return "docs"
        case "":
            return "none"
        default:
            return "other:" + ext
    }
}

func main() {
    for e in ["sl", "md", "", "png", "rs"] {
        println(Kind(e))
    }
}
