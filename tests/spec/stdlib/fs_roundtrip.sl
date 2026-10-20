// test: create, write, append, read, list, rename and remove files inside a temp dir
// status: passing
namespace spec.stdlib.fs_roundtrip

import std
import std.io

func Run(root string) void! {
    fs.MkdirAll(fs.Join(root, "sub"))?
    var p = fs.Join(root, "a.txt")
    io.WriteFile(p, "hello\n")?
    fs.AppendFile(p, "world\n")?
    println(fs.Exists(p), fs.IsDir(root), fs.IsDir(p), fs.Size(p)?)
    var lines = io.ReadLines(p)?
    println(len(lines), lines[0], lines[1])
    fs.Rename(p, fs.Join(root, "sub/b.txt"))?
    println(fs.Exists(p), io.ReadFile(fs.Join(root, "sub/b.txt"))? == "hello\nworld\n")
    var names = fs.ReadDir(fs.Join(root, "sub"))?
    println(names)
    fs.Remove(fs.Join(root, "sub/b.txt"))?
    println(len(fs.ReadDir(fs.Join(root, "sub"))?))
}

func main() {
    var root = fs.Join(fs.TempDir(), fmt.Format("slang-spec-fs-{0}", os.Pid()))
    try Run(root) catch err {
        println("error:", err)
    }
    try fs.RemoveAll(root) catch err {
        println("cleanup failed:", err)
    }
    println("cleaned:", !fs.Exists(root))
}
