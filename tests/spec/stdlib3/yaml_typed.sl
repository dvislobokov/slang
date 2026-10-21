// test: std.encoding.yaml Parse<T>/Format<T> with @yaml(name:, omitEmpty:, skip:, flow:) and @serde, round trips through Format (quoting of strings that look like other types, multi-line strings as literal blocks), maps with sorted keys, pointers and enums; error messages with lines and paths (bad indentation, duplicate keys, unterminated quotes, tabs, unknown aliases, alias bombs, type mismatches)
// status: passing
namespace spec.stdlib3.yaml_typed

import std
import std.io

enum Mode {
    Dev
    Prod
}

struct Limits {
    @yaml(name: "cpu")    Cpu    float
    @yaml(name: "memory") Memory string
}

struct Service {
    @yaml(name: "name")                   Name     string
    @yaml(name: "image")                  Image    string
    @yaml(name: "ports", flow: true)      Ports    []int
    @yaml(name: "env", omitEmpty: true)   Env      map[string]string
    @yaml(name: "limits")                 Limits   *Limits
    @yaml(name: "mode")                   Mode     Mode
    @yaml(name: "replicas")               Replicas int8
    @serde(name: "command")               Command  []string
    @yaml(skip: true)                     internal string
    @yaml(name: "notes", omitEmpty: true) Notes    string
}

struct Stack {
    @yaml(name: "version")  Version  string
    @yaml(name: "services") Services []Service
}

func main() void! {
    var st = Stack{
        Version: "3.9",
        Services: [
            Service{
                Name: "web",
                Image: "nginx:1.25",
                Ports: [80, 443],
                Env: map[string]string{"TZ": "UTC", "A": "yes"},
                Limits: &Limits{Cpu: 0.5, Memory: "512M"},
                Mode: Mode.Prod,
                Replicas: 3,
                Command: ["nginx", "-g", "daemon off;"],
                internal: "x",
                Notes: "line one\nline two\n",
            },
            Service{
                Name: "db",
                Image: "postgres",
                Ports: [],
                Mode: Mode.Dev,
                Command: ["true", "", "null", "123", " padded ", "a: b", "#hash", "- dash"],
            },
        ],
    }
    var text = encoding.yaml.Format(st)
    print(text)
    var back = encoding.yaml.Parse<Stack>(text)?
    println(
        back.Version,
        len(back.Services),
        back.Services[0].Ports,
        back.Services[0].Env["A"],
        back.Services[0].Limits.Cpu,
        back.Services[0].Mode,
        back.Services[0].Replicas,
    )
    println(
        back.Services[1].Command,
        len(back.Services[1].Command[1]),
        back.Services[1].Limits == nil,
        encoding.json.Quote(back.Services[0].Notes),
    )
    println(encoding.yaml.Format(back) == text)

    var v = encoding.yaml.ParseValue("n: 0o17\nh: 0xff\nf: -1.5e+2\nlist: [a, {b: c}]\n")?
    println(
        v.Get("n").Int()?,
        v.Get("h").Int()?,
        v.Get("f").Float()?,
        v.Get("list").Index(1).Get("b").Text(),
        v.Get("missing").IsNull(),
    )
    print(v.Format())
    var m = encoding.yaml.NewMap(["k"], [encoding.yaml.NewSeq([
        encoding.yaml.NewInt(1),
        encoding.yaml.NewString("two"),
        encoding.yaml.NewNull(),
    ])])?
    print(m.Format())
    var s = encoding.yaml.Decode<Limits>(encoding.yaml.ParseValue("cpu: 2\nmemory: 1G")?)?
    println(s.Cpu, s.Memory)

    var bad = [
        "a: 1\n  b: 2\n",
        "a: 1\na: 2\n",
        "a: \"open\n",
        "a:\n\t- x\n",
        "a: *nope\n",
        "a: 1\nb\n",
        "- a\nb: 1\n",
        "a: [1, 2\n",
        "a: b: c\n",
        "x: 1\n---\ny: 2\n",
    ]
    for b in bad {
        try encoding.yaml.ParseValue(b) catch err {
            println(err.Message())
        }
    }
    var bomb = "a: &a [x, x, x, x, x, x, x, x, x, x]\nb: &b [*a, *a, *a, *a, *a, *a, *a, *a, *a, *a]\nc: &c [*b, *b, *b, *b, *b, *b, *b, *b, *b, *b]\nd: &d [*c, *c, *c, *c, *c, *c, *c, *c, *c, *c]\ne: &e [*d, *d, *d, *d, *d, *d, *d, *d, *d, *d]\nf: &f [*e, *e, *e, *e, *e, *e, *e, *e, *e, *e]\ng: &g [*f, *f, *f, *f, *f, *f, *f, *f, *f, *f]\n"
    try encoding.yaml.ParseValue(bomb) catch err {
        println(err.Message())
    }
    try encoding.yaml.Parse<Stack>(
        "version: 3\nservices:\n  - name: x\n    replicas: 1000\n",
    ) catch err {
        println(err.Message())
    }
    try encoding.yaml.Parse<Stack>("services:\n  - ports: 80\n") catch err {
        println(err.Message())
    }
    try encoding.yaml.Parse<Stack>("services:\n  - mode: Test\n") catch err {
        println(err.Message())
    }
    println(len(encoding.yaml.ParseAll("a: 1\n---\nb: 2\n---\n")?))
}
