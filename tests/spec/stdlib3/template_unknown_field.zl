// error: invalid template: template: template:2: can't evaluate field Nmae in type Item
// status: passing
namespace spec.stdlib3.template_unknown_field

import std
import std.io

struct Item {
    Name string
}

struct Page {
    Title string
    Items []Item
}

// template.Parse<T> checks a literal template against T when compiling.
func main() void! {
    var t = text.template.Parse<Page>("{{.Title}}\n{{range .Items}}{{.Nmae}}{{end}}")?
    println(t.Execute(Page{Title: "x"})?)
}
