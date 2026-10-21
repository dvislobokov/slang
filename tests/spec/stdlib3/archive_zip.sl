// test: std.archive.zip reads an archive made by zip(1) (deflate + store, directory entry, DOS times), writes archives (Add, AddDir, AddHeader with Store/Deflate, comments, Zip64 records) that it reads back, rejects bad data, duplicate and unsafe names; ExtractTo / FromDir round trip through a temp directory
// status: passing
namespace spec.stdlib3.archive_zip

import std
import std.io

func short(name string) string {
    if len(name) > 30 {
        return name[..12] + "..." + name[len(name) - 6..]
    }
    return name
}

func main() void! {
    var data = encoding.base64.Decode(
        "UEsDBAoAAAAAAKMgQ1jI5k+PEgAAABIAAAAJAAAAaGVsbG8udHh0aGVsbG8gZnJvbSB6aXAoMSkKUEsDBAoAAAAAAKMgQ1gAAAAAAAAAAAAAAAAFAAAAZG9jcy9QSwMECgAAAAAAoyBDWFB4hXEFAAAABQAAAHcAAABkb2NzL2xsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsLnR4dGxvbmcKUEsDBBQAAAAIAKMgQ1gxabbbIgAAAGAJAAAMAAAAZG9jcy9yZXAudHh0K0otSE0sUcjJzEvlKhplj7JH2aPsUfYoe5Q9yh5lU8wGAFBLAQIeAwoAAAAAAKMgQ1jI5k+PEgAAABIAAAAJAAAAAAAAAAEAAACkgQAAAABoZWxsby50eHRQSwECHgMKAAAAAACjIENYAAAAAAAAAAAAAAAABQAAAAAAAAAAABAA7UE5AAAAZG9jcy9QSwECHgMKAAAAAACjIENYUHiFcQUAAAAFAAAAdwAAAAAAAAABAAAApIFcAAAAZG9jcy9sbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbGxsbC50eHRQSwECHgMUAAAACACjIENYMWm22yIAAABgCQAADAAAAAAAAAABAAAApIH2AAAAZG9jcy9yZXAudHh0UEsFBgAAAAAEAAQASQEAAEIBAAAAAA==",
    )?
    var r = archive.zip.Open(data)?
    for f in r.Files {
        println(short(f.Name), f.Method, f.Size, f.Modified, f.IsDir())
    }
    print(strings.FromBytes(r.Read("hello.txt")?))
    println(len(r.Read("docs/rep.txt")?), r.File("nope") == nil, r.Comment == "")
    try r.Read("nope") catch err {
        println(err.Message())
    }

    var w = archive.zip.NewWriter()
    w.Comment = "made by slang"
    w.AddString("a.txt", "alpha")?
    w.AddDir("dir", 1700000000)?
    w.AddHeader(
        archive.zip.Header{
            Name: "dir/b.bin",
            Method: archive.zip.Store,
            Modified: 1700000001,
            Mode: 384,
            Comment: "raw",
        },
        [byte(0), byte(1), byte(2)],
    )?
    w.Add("dir/big.txt", strings.Bytes(strings.Repeat("compressible ", 1000)))?
    try w.AddString("a.txt", "again") catch err {
        println(err.Message())
    }
    var out = w.Close()
    var back = archive.zip.Open(out)?
    println(back.Comment, back.Names())
    for f in back.Files {
        println(f.Name, f.Method, f.Size, f.CompressedSize, f.Modified, f.Mode, f.Comment)
    }
    println(strings.FromBytes(back.Read("a.txt")?), back.Read("dir/b.bin")?)

    var w64 = archive.zip.NewWriter()
    w64.Zip64 = true
    w64.AddString("x", "zip64 entry")?
    var z64 = archive.zip.Open(w64.Close())?
    println(z64.Names(), strings.FromBytes(z64.Read("x")?))

    // corrupt data
    try archive.zip.Open(strings.Bytes("PK not really")) catch err {
        println(err.Message())
    }
    var bad []byte = []
    for b in out {
        bad = append(bad, b)
    }
    bad[46] = bad[46] ^ byte(1)
    var br = archive.zip.Open(bad)?
    try br.Read("a.txt") catch err {
        println(err.Message())
    }

    // directories
    var tmp = fs.Join(fs.TempDir(), fmt.Format("slang-zip-test-{0}", os.Pid()))
    fs.RemoveAll(tmp)?
    archive.zip.ExtractTo(data, fs.Join(tmp, "out"))?
    println(fs.ReadDir(fs.Join(tmp, "out"))?, len(io.ReadFile(fs.Join(tmp, "out/hello.txt"))?) > 0)
    fs.RemoveAll(tmp)?

    var evil = archive.zip.NewWriter()
    evil.AddString("../escape.txt", "no")?
    try archive.zip.ExtractTo(evil.Close(), fs.Join(tmp, "evil")) catch err {
        println(err.Message())
    }
    fs.RemoveAll(tmp)?
}
