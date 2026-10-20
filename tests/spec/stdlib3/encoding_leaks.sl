// test: std.encoding.{csv,xml,yaml,toml} and std.archive.{zip,tar} free everything they allocate, also on errors
// status: passing
// run-flags: --leak-check
namespace spec.stdlib3.encoding_leaks

import std
import std.io

struct Row {
    @csv(name: "n") N int
    @xml(attr: true) id string
    Tags []string
    Child *Row
}

func main() {
    for i in 0..2 {
        var rows = try encoding.csv.Read<Row>("n,id,Tags,Child\n1,a,x,\n") else []
        var bad = try encoding.csv.Read<Row>("n\nx\n") else []
        var x = try encoding.xml.Parse<Row>("<Row id=\"q\"><N>4</N><Tags>a</Tags><Child id=\"c\"><N>5</N></Child></Row>") else Row{}
        var xs = encoding.xml.Format(x)
        var xe = try encoding.xml.ParseValue("<a><b></a>") else encoding.xml.Value{}
        var y = try encoding.yaml.ParseValue("a: &x [1, {b: c}]\nb: *x\nc: |\n  text\n") else encoding.yaml.Value{}
        var ys = y.Format()
        var ye = try encoding.yaml.ParseValue("a: [1,\n") else encoding.yaml.Value{}
        var t = try encoding.toml.ParseDocument("[a]\nb = 1 # c\n[[d]]\ne = \"f\"\n") else encoding.toml.Document{}
        try t.Set("a.c", encoding.toml.NewInt(2)) catch err {
            println(err.Message())
        }
        var te = try encoding.toml.ParseValue("a = 1\na = 2\n") else encoding.toml.Value{}
        var z = archive.zip.NewWriter()
        try z.AddString("f.txt", "zip data zip data zip data") catch err {
            println(err.Message())
        }
        var zr = try archive.zip.Open(z.Close()) else archive.zip.Reader{}
        var tw = archive.tar.NewWriter()
        try tw.AddString(strings.Repeat("long/", 40) + "f", "tar data") catch err {
            println(err.Message())
        }
        var tr = try archive.tar.Read(tw.Close()) else []
        println(i, len(rows), len(bad), x.Child.N, len(xs) > 0, xe.Name.Local == "", len(ys) > 0, ye.IsNull(), len(t.String()), te.Len(), len(zr.Files), len(tr))
    }
}
