// test: gzip std.io adapters: StreamWriter over io.BytesWriter with a header, StreamReader over io.BytesReader (ReadAllString), truncated input reports an error
// status: passing
// run-flags: --leak-check
namespace spec.stdlib3.compress_io

import std

func main() {
    var w = io.NewBytesWriter()
    var gw = compress.gzip.NewStreamWriter(w, 6)
    var h = compress.gzip.Header{Name: "a.txt"}
    gw.SetHeader(h)
    for i in 0..1000 {
        try gw.Write(strings.Bytes(fmt.Format("line {0}\n", i))) catch e {
            io.println(e)
            return
        }
    }
    try gw.Close() catch e {
        io.println(e)
        return
    }
    var gz = w.Bytes()
    var back = try compress.gzip.Decompress(gz) catch e {
        io.println(e)
        return
    }
    io.println(len(back), strings.HasSuffix(strings.FromBytes(back), "line 999\n"))
    var r = compress.gzip.NewStreamReader(io.NewBytesReader(gz))
    var s = try io.ReadAllString(r) catch e {
        io.println(e)
        return
    }
    io.println(len(s), r.Header().Name)
    var bad = compress.gzip.NewStreamReader(io.NewBytesReader(compress.rtCopy(gz, 0, len(gz) - 5)))
    try io.ReadAllString(bad) catch e {
        io.println("err:", e)
    }
}
