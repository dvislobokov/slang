// test: @embed puts file contents into string/[]byte/const globals and an embed.FS from a directory or glob, at compile time
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.embed_files

import std
import std.io

@embed("greeting.txt")
var Greeting string

@embed("blob.bin")
var Blob []byte

@embed("greeting.txt")
const Banner = ""

@embed("assets")
var Assets embed.FS

@embed("assets/*.txt")
var Texts embed.FS

func main() {
    print(Greeting)
    println(len(Greeting), Blob, len(Banner), strings.HasPrefix(Banner, "Hello"))
    println(Assets.Names(), Assets.Len(), Texts.Names())
    println(try Assets.ReadString("assets/sub/c.txt") else "?", try Assets.ReadString("./assets/a.txt") else "?", Assets.Has("assets/b.md"), Texts.Has("assets/b.md"))
    println(Assets.List("assets/sub"))
    try {
        _ = Assets.ReadFile("nope")?
    } catch err {
        println(err, errors.Is(err, fs.ErrNotFound))
    }
}
