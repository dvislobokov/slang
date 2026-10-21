// test: Sprintf verbs %d %s %v %.2f %x %q with negative numbers, unicode and padding
// status: passing
namespace spec.strings2.sprintf_verbs

import std
import std.io

func main() {
    println(fmt.Sprintf("[%d] [%5d] [%-5d] [%05d]", -42, -42, -42, 42))
    println(fmt.Sprintf("[%s] [%8s] [%-8s]", "héllo", "ab", "ab"))
    println(fmt.Sprintf("%v %v %v %v", 1, "s", false, 2.5))
    println(fmt.Sprintf("%.2f %.0f %.3f %.2f", 3.14159, 2.5, -1.0, 100.0))
    println(fmt.Sprintf("%x %x %x", 255, 4096, "hi"))
    println(fmt.Sprintf("%q %q", "tab\there", "日本"))
    println(fmt.Sprintf("100%% %d%%", 5))
}
