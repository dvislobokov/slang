namespace examples.errors_trace

import std
import std.io

const ErrMissing = error("missing key")

struct Store {
    Data map[string]string

    func Get(key string) string! {
        if key == "" {
            return ErrMissing
        }
        var v = this.Data[key]
        if v == "" {
            return error("no value for " + key)
        }
        return v
    }
}

func Port(s Store) int! {
    var text = s.Get("port")?("reading port")
    return strings.ToInt(text)?
}

func Start(s Store) void! {
    var p = Port(s)?
    println("listening on", p)
}

// Prints one frame per line without the directory part of the file name,
// so the output does not depend on where the example is built.
func Show(err error) {
    println("error:", err)
    for f in errors.Frames(err) {
        var ctx = ""
        if f.Context != "" {
            ctx = " (" + f.Context + ")"
        }
        println(fmt.Format("  {0}:{1} {2}{3}", fs.Base(f.File), f.Line, f.Func, ctx))
    }
}

func main() {
    var ok = Store{Data: map[string]string{"port": "8080"}}
    try Start(ok) catch err {
        Show(err)
    }
    try Start(Store{Data: map[string]string{"port": "eighty"}}) catch err {
        Show(err)
    }
    try Start(Store{Data: map[string]string{}}) catch err {
        Show(err)
    }
    var s = Store{}
    try s.Get("") catch err {
        Show(err)
        println("is ErrMissing:", errors.Is(err, ErrMissing))
        println(len(strings.Split(errors.Trace(err), "\n")), "trace lines")
    }
}
