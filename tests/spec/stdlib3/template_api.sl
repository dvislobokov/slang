// test: std.text.template and std.html.template API: ParseWith custom functions (errors propagate from Execute), ParseNamed, ExecuteTemplate and Defined with {{define}}, the dynamic Value of maps with int keys (sorted), any and enum fields, run-time checking of non-literal sources, Execute errors (index out of range), trusted html.template.HTML/URL/JS values, nested templates in HTML
// status: passing
namespace spec.stdlib3.template_api

import std
import std.io

enum Tier {
    Free
    Pro
}

struct Row {
    Id int
    Name string
}

struct Report {
    Title string
    Rows []Row
    Scores map[int]string
    Extra any
    Tier Tier
    Initial char
    Body html.template.HTML
    Link html.template.URL
    Code html.template.JS
}

func main() void! {
    var funcs = text.template.Funcs()
    funcs["shout"] = func(args []text.template.Value) text.template.Value! {
        return text.template.Str(strings.ToUpper(args[0].Text()) + "!")
    }
    funcs["fail"] = func(args []text.template.Value) text.template.Value! {
        return error("deliberate failure")
    }
    funcs["sum"] = func(args []text.template.Value) text.template.Value! {
        var t = 0
        for a in args {
            t += a.Int()?
        }
        return text.template.Int(t)
    }
    var r = Report{Title: "q3", Rows: [Row{Id: 1, Name: "a"}, Row{Id: 2, Name: "b"}], Scores: map[int]string{10: "ten", 2: "two", 33: "x"}, Extra: 42, Tier: Tier.Pro, Initial: 'Q', Body: html.template.HTML{Text: "<em>trusted</em>"}, Link: html.template.URL{Text: "javascript:void(0)"}, Code: html.template.JS{Text: "{a: 1}"}}
    var t = text.template.ParseWith<Report>("{{.Title | shout}} {{sum 1 2 3}} {{range $k, $v := .Scores}}{{$k}}={{$v}} {{end}}{{.Extra}} {{.Tier}} {{.Initial}}", funcs)?
    println(t.Execute(r)?)
    var bad = text.template.ParseWith<Report>("{{fail}}", funcs)?
    try bad.Execute(r) catch err {
        println(err.Message())
    }
    var named = text.template.ParseNamed<Report>("report", "{{define \"row\"}}<{{.Id}}:{{.Name}}>{{end}}{{define \"head\"}}[{{.Title}}]{{end}}{{template \"head\" .}}{{range .Rows}}{{template \"row\" .}}{{end}}")?
    println(named.Name(), named.Defined(), named.Execute(r)?, named.ExecuteTemplate("head", r)?)
    try named.ExecuteTemplate("row", r) catch err {
        println(err.Message())
    }
    try named.ExecuteTemplate("nope", r) catch err {
        println(err.Message())
    }
    // a dynamic source is checked when parsed
    var src = "{{.Rows.Count}}"
    try text.template.Parse<Report>(src) catch err {
        println(err.Message())
    }
    var oob = text.template.Parse<Report>("{{index .Rows 5}}")?
    try oob.Execute(r) catch err {
        println(err.Message())
    }
    var h = html.template.Parse<Report>("{{define \"r\"}}<li>{{.Name}}</li>{{end}}<div>{{.Body}}</div><a href=\"{{.Link}}\">{{.Title}}</a><script>var c = {{.Code}};</script><ul>{{range .Rows}}{{template \"r\" .}}{{end}}</ul>")?
    println(h.Execute(r)?)
}
