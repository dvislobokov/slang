// test: std.text.template / std.html.template free everything (parse, check errors, execution, custom functions, escaping)
// status: passing
// run-flags: --leak-check
namespace spec.stdlib3.template_leaks

import std
import std.io

struct Item {
    Name string
    Tags []string
}

struct Page {
    Title string
    Items []Item
    Meta map[string]int
}

func main() {
    var p = Page{Title: "<t>", Items: [Item{Name: "a", Tags: ["x"]}], Meta: map[string]int{"k": 1}}
    for i in 0..3 {
        var t = try text.template.Parse<Page>("{{.Title}}{{range $i, $e := .Items}}{{$i}}{{$e.Name}}{{join .Tags \",\"}}{{end}}{{printf \"%05.1f\" 3.14159}}{{index .Meta \"k\"}}") catch err {
            println(err.Message())
            continue
        }
        var out = try t.Execute(p) else ""
        var h = try html.template.Parse<Page>("<a href=\"/x?q={{.Title}}\" title={{.Title}}>{{.Title}}</a><script>var x = {{.Items}};</script>") catch err {
            continue
        }
        println(i, out, try h.Execute(p) else "")
        var src = "{{.Nope}}"
        var bad = try text.template.Parse<Page>(src) catch err {
            println(err.Message())
            continue
        }
        println(bad.Name())
    }
}
