// test: std.web DI: singletons (created once by Build, in order), scoped services (one per request, dropped with it), transient services (new per resolution), constructor injection by static New, @inject fields, interfaces registered with <I, T>, factories, instances, web.Resolve<T>() and Context.Resolve<T>() inside handlers, a missing registration at run time
// status: passing
// run-flags: --leak-check
namespace spec.web.di_scopes

import std.io
import std.web

interface Greeter {
    func Greet(name string) string
}

struct Polite {
    func Greet(name string) string {
        return "Good day, " + name
    }
}

struct Counter {
    n int
}

struct Config {
    Prefix string
}

struct Db {
    id int

    static func New(c *Config) *Db {
        println("  new Db for", c.Prefix)
        return new Db{id: 1}
    }
}

struct RequestState {
    id int

    func Drop() {
        println("  drop RequestState", this.id)
    }
}

struct Stamp {
    n int
}

struct Repo {
    @inject db    *Db
    @inject state *RequestState
    @inject stamp *Stamp
    cache map[string]int
}

@route("/di")
struct DiController {
    repo    *Repo
    greeter Greeter
    state   *RequestState
    counter *Counter
    @inject(skip: true) unused *Counter
    label string

    @get("{name}")
    func Get(name string, s2 *Stamp) string {
        this.counter.n += 1
        this.state.id = this.counter.n
        var same = this.repo.state == this.state
        return std.fmt.Format(
            "{0} | request {1} | same scope {2} | transient differs {3} | unused nil {4} | db {5}",
            this.greeter.Greet(name),
            this.counter.n,
            same,
            this.repo.stamp != s2,
            this.unused == nil,
            this.repo.db.id,
        )
    }
}

struct Missing {
    n int
}

async func main() void! {
    var app = NewApp()
    app.Services.AddInstance(new Config{Prefix: "test"})
    app.Services.AddSingleton<Counter>()
    app.Services.AddSingleton<Db>()
    app.Services.AddScoped<RequestState>()
    app.Services.AddTransient<Stamp>()
    app.Services.AddScoped<Repo>()
    app.Services.AddSingleton<Greeter, Polite>()
    app.Services.AddTransientFactory<string>(func(s *Scope) string! {
        var c = s.Get<*Config>()?
        return c.Prefix + "-value"
    })
    app.MapController<DiController>()
    app.MapGet("/resolve", func() string! {
        var g = Resolve<Greeter>()?
        var v = Resolve<string>()?
        return g.Greet(v)
    })
    app.MapGet("/ctx", func(c *Context) string! {
        var a = c.Resolve<*RequestState>()?
        var b = c.Resolve<*RequestState>()?
        a.id = 100
        return std.fmt.Format("{0}", a == b)
    })
    app.MapGet("/missing", func() int! {
        var m = Resolve<*Missing>()?
        return m.n
    })
    println("keys:", app.Services.Keys())
    println("build:")
    var c = TestServer(app)?
    for path in ["/di/ann", "/di/bob", "/resolve", "/ctx", "/missing"] {
        println("GET", path)
        var r = await c.Get(path)
        println(" ", r.Status, r.Body)
    }
    var lt = app.Services.LifetimeOf("*spec.web.di_scopes.RequestState")?
    println("lifetime:", lt, app.Services.Has<*Db>(), app.Services.Has<*Missing>())
}
