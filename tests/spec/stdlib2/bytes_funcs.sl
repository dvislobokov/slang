// test: std.bytes functions over []byte and bytes.Buffer
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.bytes_funcs

import std
import std.io

func s(b []byte) string {
    return strings.FromBytes(b)
}

func b(x string) []byte {
    return strings.Bytes(x)
}

func main() {
    println(bytes.Equal(b("abc"), b("abc")), bytes.Equal(b("abc"), b("abd")), bytes.Compare(b("a"), b("b")), bytes.Compare(b("ab"), b("a")))
    println(bytes.Index(b("chicken"), b("ken")), bytes.Index(b("chicken"), b("dmr")), bytes.LastIndex(b("go gopher"), b("go")), bytes.IndexByte(b("abc"), 99))
    println(bytes.Contains(b("seafood"), b("foo")), bytes.Count(b("cheese"), b("e")), bytes.Count(b("five"), b("")))
    println(bytes.HasPrefix(b("golang"), b("go")), bytes.HasSuffix(b("golang"), b("ng")), bytes.HasPrefix(b("g"), b("go")))
    var parts = bytes.Split(b("a,b,,c"), b(","))
    println(len(parts), s(parts[0]), s(parts[3]), s(bytes.Join(parts, b("|"))))
    println(len(bytes.Split(b("héllo"), b(""))), len(bytes.Fields(b("  a b   c "))))
    println(s(bytes.Repeat(b("ab"), 3)), s(bytes.Replace(b("oink oink"), b("k"), b("ky"))), s(bytes.Replace(b("ab"), b(""), b("-"))))
    println("[" + s(bytes.Trim(b("  hi \n"))) + "]", s(bytes.TrimLeft(b("xxhix"), "x")), s(bytes.TrimRight(b("xxhix"), "x")))
    println(s(bytes.TrimPrefix(b("prefix-body"), b("prefix-"))), s(bytes.TrimSuffix(b("a.go"), b(".go"))), s(bytes.TrimSuffix(b("a.go"), b(".sl"))))
    println(s(bytes.ToUpper(b("héllo"))), s(bytes.ToLower(b("HÉLLO"))), bytes.EqualFold(b("Go"), b("GO")), bytes.Runes(b("hé")))

    var buf = bytes.Buffer{}
    _ = buf.WriteString("hello ")
    _ = buf.Write(b("world"))
    buf.WriteByte(33)
    buf.WriteRune('é')
    println(buf.String(), buf.Len())
    var p = [byte(0), 0, 0, 0, 0]
    var n = try buf.Read(p) else -1
    println(n, s(p), buf.String())
    println(try buf.ReadString(111) else "?", s(buf.Next(2)), buf.Len())
    println(try buf.ReadByte() else 0)
    buf.Truncate(1)
    println(buf.String())
    buf.Reset()
    println(buf.Len(), try buf.ReadByte() else 0, try buf.Read(p) else -1)
    var lines = bytes.NewBufferString("one\ntwo\nthree")
    for lines.Len() > 0 {
        var line = try lines.ReadString(10) else "<eof>"
        print(strings.TrimSuffix(line, "\n"), ";")
    }
    println()
    var nb = bytes.NewBuffer(b("xy"))
    println(nb.String(), s(nb.Bytes()))
}
