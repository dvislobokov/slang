// test: std.regex frees everything it allocates (compile, DFA cache reset, Pike VM, groups, replace, split, errors)
// status: passing
// run-flags: --leak-check
namespace spec.stdlib3.regex_leaks

import std
import std.io

func main() {
    for i in 0..3 {
        var re = regex.Pattern(`(?P<k>\w+)=(?P<v>[^;]*);?`)
        var g = re.Groups("a=1;b=2")
        println(i, g.Get("k"), g.Get("v"), re.Replace("x=1;y=2", "$v:$k "), re.Split("q a=1 r"))
        try regex.Compile("(bad") catch err {
            println(err.Message())
        }
        // many states: force a cache reset
        var big = regex.Pattern(`[a-q][^u-z]{13}x`)
        var sb = strings.Builder{}
        for k in 0..3000 {
            sb.Write(fmt.Format("{0}", k * 7919 % 10007))
            sb.Write("abcdefghijklmnop")
        }
        println(len(big.FindAll(sb.String())) >= 0)
    }
}
