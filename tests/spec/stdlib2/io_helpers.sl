// test: std.io readers/writers (String/Bytes readers, Limit, Multi, Tee, Pipe, Discard), bufio-style Buffered* and Scanner, binary.Read/Write
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.io_helpers

import std
import std.io
import std.encoding

struct Point {
    X int32
    Y int32
}

// net.Conn satisfies the async stream interfaces (checked at compile time).
func asAsync(c net.Conn) io.AsyncReader {
    var w io.AsyncWriter = c
    _ = w
    return c
}

async func pipeDemo() void! {
    var p = io.Pipe()
    var r = p.Reader
    var w = p.Writer
    var reader = task.Spawn(async func() string {
        var all = ""
        for {
            var chunk = try await r.Recv(4) else []
            if len(chunk) == 0 {
                return all
            }
            all += strings.FromBytes(chunk)
        }
    })
    _ = w.WriteString("abcdef")?
    await task.Delay(1)
    _ = w.WriteString("gh")?
    w.Close()?
    println("pipe:", await reader)
    var p2 = io.Pipe()
    var buf = [byte(0), 0]
    try {
        _ = p2.Reader.Read(buf)?
    } catch err {
        println(err)
    }
}

async func main() void! {
    var sr = io.NewStringReader("hello world")
    var five = io.ReadN(sr, 5)?
    println(strings.FromBytes(five), sr.Len(), try sr.ReadByte() else 0)
    _ = sr.Seek(0, io.SeekStart)?
    var lr = io.LimitReader(sr, 7)
    println(io.ReadAllString(lr)?, lr.N)
    var parts []io.Reader = [io.NewStringReader("ab"), io.NewStringReader(""), io.NewBytesReader(strings.Bytes("cd"))]
    var mr = io.MultiReader(parts)
    println(io.ReadAllString(mr)?)
    var log = io.NewBytesWriter()
    var tee = io.TeeReader(io.NewStringReader("teed"), log)
    var sink = io.NewBytesWriter()
    println(io.Copy(sink, tee)?, sink.String(), log.String())
    var a = io.NewBytesWriter()
    var b = io.NewBytesWriter()
    var ws []io.Writer = [a, b]
    var both = io.MultiWriter(ws)
    _ = io.WriteString(both, "x2")?
    println(a.String(), b.String(), io.WriteString(io.Discard(), "gone")?)
    println(io.CopyN(sink, io.NewStringReader("12345"), 3)?, sink.String())
    try {
        _ = io.CopyN(sink, io.NewStringReader("1"), 3)?
    } catch err {
        println("copyn:", err, errors.Is(err, io.EOF))
    }
    var full = [byte(0), 0, 0, 0]
    try {
        _ = io.ReadFull(io.NewStringReader("ab"), full)?
    } catch err {
        println("readfull:", err)
    }

    var br = io.NewBufferedReaderSize(io.NewStringReader("line one\r\nline two\nlast"), 16)
    println(strings.FromBytes(br.Peek(4)?), br.Buffered())
    println(br.ReadLine()?, "|", br.ReadString(32)?, "|", br.ReadLine()?, "|", try br.ReadLine() else "EOF?", "|", try br.ReadLine() else "EOF")
    var rr = io.NewBufferedReader(io.NewStringReader("é!"))
    println(string(rr.ReadRune()?), string(rr.ReadRune()?), try rr.ReadByte() else 0)

    var out = io.NewBytesWriter()
    var bw = io.NewBufferedWriterSize(out, 16)
    _ = bw.WriteString("buffered ")?
    bw.WriteByte(33)?
    println(out.Len(), bw.Buffered(), bw.Available())
    bw.Flush()?
    _ = bw.WriteString("a long string that bypasses the buffer")?
    println(out.String())

    println(io.Words(io.NewStringReader("  the quick\tbrown\n fox "))?)
    var lines = io.Lines(io.NewStringReader("a\nb\r\n\nc"))?
    println(len(lines), lines)
    var sc = io.NewScanner(io.NewStringReader("aé"))
    sc.Split(io.ScanRunes)
    while_runes(sc)
    var big = strings.Repeat("x", 70000)
    var sc2 = io.NewScanner(io.NewStringReader(big))
    println(sc2.Scan(), sc2.Err())

    var bin = io.NewBytesWriter()
    binary.Write(bin, binary.ByteOrder.BigEndian, Point{X: 1, Y: -2})?
    println(bin.Bytes())
    var back = binary.Read<Point>(io.NewBytesReader(bin.Bytes()), binary.ByteOrder.BigEndian)?
    println(back.X, back.Y)
    await pipeDemo()?
}

func while_runes(sc *io.Scanner) {
    var s = sc
    for s.Scan() {
        print(s.Text(), ";")
    }
    println()
}
