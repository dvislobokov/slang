// test: std.encoding.toml Parse<T>/Format<T> with @toml(name:, omitEmpty:, skip:, inline:) and @serde (nested tables, arrays of tables, maps, datetimes as strings), and Document round-trip editing of a package manifest: Set replaces a value in place keeping its comment, adds keys to an existing table or a new [table], Delete removes a line; untouched text stays byte-identical; error messages carry line numbers
// status: passing
namespace spec.stdlib3.toml_document

import std
import std.io

struct Package {
    @toml(name: "name") Name string
    @toml(name: "version") Version string
    @toml(name: "authors", omitEmpty: true) Authors []string
    @toml(name: "edition") Edition int
}

struct Dep {
    @toml(name: "version") Version string
    @toml(name: "features", omitEmpty: true) Features []string
}

struct Target {
    @toml(name: "name") Name string
    @toml(name: "path") Path string
}

struct Manifest {
    @toml(name: "package") Package Package
    @toml(name: "dependencies") Dependencies map[string]Dep
    @toml(name: "bin") Bins []Target
    @toml(name: "release", omitEmpty: true) Released string
    @toml(name: "meta", inline: true) Meta map[string]int
    @toml(skip: true) cache string
}

func main() void! {
    var m = Manifest{
        Package: Package{Name: "demo", Version: "0.1.0", Authors: ["Ann <ann@x.org>"], Edition: 2026},
        Dependencies: map[string]Dep{"json": Dep{Version: "1.2"}, "http": Dep{Version: "2.0", Features: ["tls", "h2"]}},
        Bins: [Target{Name: "demo", Path: "src/main.sl"}, Target{Name: "tool", Path: "src/tool.sl"}],
        Released: "2026-10-20T10:00:00Z",
        Meta: map[string]int{"b": 2, "a": 1},
    }
    var text = encoding.toml.Format(m)
    print(text)
    var back = encoding.toml.Parse<Manifest>(text)?
    println(back.Package.Name, back.Package.Edition, len(back.Dependencies), back.Dependencies["http"].Features, back.Bins[1].Path, back.Released, back.Meta["b"])
    println(encoding.toml.Format(back) == text)
    var dt = encoding.toml.ParseValue("t = 1979-05-27 07:32:00z\nd = 1979-05-27\nlt = 07:32:00.5\n")?
    println(dt.Get("t").DateTime()?, dt.Get("t").DateKind(), dt.Get("d").DateKind(), dt.Get("lt").DateKind(), dt.Get("lt").DateTime()?)

    var src = "# Slang package manifest\n[package]\nname = \"demo\"   # the package name\nversion = \"0.1.0\"\n\n[dependencies]\n# pinned\njson = \"1.2\"\nhttp = { version = \"2.0\", features = [\"tls\"] }\n\n[[bin]]\nname = \"demo\"\n"
    var doc = encoding.toml.ParseDocument(src)?
    println(doc.Get("package.name").String()?, doc.Has("dependencies.json"), doc.Has("dependencies.nope"))
    doc.Set("package.version", encoding.toml.NewString("0.2.0"))?
    doc.Set("package.name", encoding.toml.NewString("renamed"))?
    doc.Set("dependencies.regex", encoding.toml.NewString("0.9"))?
    doc.Set("dependencies.\"my.pkg\"", encoding.toml.NewTable(["version", "optional"], [encoding.toml.NewString("1.0"), encoding.toml.NewBool(true)], true)?)?
    doc.Set("dev-dependencies.test", encoding.toml.NewString("1"))?
    doc.Set("license", encoding.toml.NewString("MIT"))?
    println(doc.Delete("dependencies.json")?, doc.Delete("dependencies.json")?)
    print(doc.String())
    var tree = doc.Tree()?
    println(tree.At("dependencies.\"my.pkg\".optional").Bool()?, tree.Get("bin").Index(0).Get("name").String()?)

    for bad in ["[package]\nname = \"x\"\nname = \"y\"\n", "a = [1,\n  2,\n  3\n", "x = 1\ny = what\n"] {
        try encoding.toml.ParseValue(bad) catch err {
            println(err.Message())
        }
    }
    try encoding.toml.Parse<Manifest>("[package]\nedition = \"new\"\n") catch err {
        println(err.Message())
    }
}
