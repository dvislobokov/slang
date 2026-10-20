namespace bench.regex

import std
import std.io

// Finds all e-mail addresses in a generated 10 MB text (same text as main.go).
func main() {
    var words = ["lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do", "eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "x@y"]
    var names = ["john.smith", "alice", "bob_42", "carol+news", "dave-ops"]
    var hosts = ["example.com", "mail.example.org", "test.io", "uni.edu", "corp.co.uk"]
    var b = strings.Builder{}
    var seed = 12345
    var n = 0
    for n < 10000000 {
        seed = (seed * 1103515245 + 12345) % 2147483648
        var r = seed >> 8
        var w = ""
        if r % 50 == 0 {
            w = names[r % 5] + "@" + hosts[(r >> 3) % 5]
        } else {
            w = words[r % 20]
        }
        b.Write(w)
        b.Write(" ")
        n += len(w) + 1
    }
    var text = b.String()
    var re = regex.Pattern(`[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}`)
    var t0 = time.NowNanos()
    var all = re.FindAll(text)
    var total = 0
    for m in all {
        total += len(m.Text)
    }
    var ms = (time.NowNanos() - t0) / 1000000
    println(len(text), len(all), total)
    io.Eprintln(fmt.Format("regex: {0} ms", ms))
}
