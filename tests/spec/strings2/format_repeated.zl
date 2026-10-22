// test: fmt.Format replaces repeated and reordered placeholders; other text, including $ and lone braces, is copied
// status: passing
namespace spec.strings2.format_repeated

import std
import std.io

func main() {
    println(fmt.Format("{0}-{1}-{0}-{0}", "a", 2))
    println(fmt.Format("{1}{0}{1}", "x", 'é'))
    println(fmt.Format("$ {0} costs ${1}", "tea", 1.5))
    println(fmt.Format("{0} {1} {2}", true, [1, 2], -3))
    println(fmt.Format("no placeholders"))
}
