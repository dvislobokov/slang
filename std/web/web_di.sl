// The DI container. A service is keyed by the full name of its type
// (reflect.TypeOf<T>().FullName): AddSingleton<T>() / AddScoped<T>() /
// AddTransient<T>() register `*T` (built by `static func New(deps...)`
// or by @inject fields; the compiler generates the factory, sema/web.rs),
// AddSingleton<I, T>() registers interface I implemented by *T,
// AddInstance(x) registers the static type of x, and the Add*Factory<T>
// methods take a `func(*web.Scope) T!`.
//
// Lifetimes: singletons are created once by App.Build (in registration
// order) and shared; scoped services once per request scope (the scope
// is released with the request, so their Drop runs then); transient
// services on every resolution.
namespace std.web

enum Lifetime {
    Singleton
    Scoped
    Transient
}

// A resolved value of type T (stored as `any`).
struct box<T> {
    v T
}

func wrap<T>(v T) any {
    return new box<T>{v: v}
}

// The key of a service type.
func keyOf<T>() string {
    return std.reflect.TypeOf<T>().FullName
}

struct registration {
    key string
    lifetime Lifetime
    factory func(*Scope) any!
    deps []string
}

// Services is the container of an App (app.Services).
struct Services {
    regs map[string]*registration
    order []string
    singletons map[string]any
    app weak *App

    // Registers a factory for key (the compiler-generated Add* methods and
    // the Add*Factory methods end here). A later registration replaces an
    // earlier one.
    func register(key string, lifetime Lifetime, f func(*Scope) any!, deps []string) *Services {
        if this.regs[key] == nil {
            this.order = append(this.order, key)
        }
        this.regs[key] = new registration{key: key, lifetime: lifetime, factory: f, deps: deps}
        return this
    }

    // Registers v (of its static type T) as a singleton.
    func AddInstance<T>(v T) *Services {
        var x = v
        return this.register(keyOf<T>(), Lifetime.Singleton, func(s *Scope) any! {
            return wrap<T>(x)
        }, [])
    }

    func AddSingletonFactory<T>(f func(*Scope) T!) *Services {
        return this.register(keyOf<T>(), Lifetime.Singleton, factoryOf<T>(f), [])
    }

    func AddScopedFactory<T>(f func(*Scope) T!) *Services {
        return this.register(keyOf<T>(), Lifetime.Scoped, factoryOf<T>(f), [])
    }

    func AddTransientFactory<T>(f func(*Scope) T!) *Services {
        return this.register(keyOf<T>(), Lifetime.Transient, factoryOf<T>(f), [])
    }

    // Whether T is registered.
    func Has<T>() bool {
        return this.regs[keyOf<T>()] != nil
    }

    // The registered service keys in registration order.
    func Keys() []string {
        return this.order
    }

    // The lifetime of a registered key.
    func LifetimeOf(key string) Lifetime! {
        var r = this.regs[key]
        if r == nil {
            return error("web: no service registered for " + key)
        }
        return r.lifetime
    }

    func canResolve(key string) bool {
        return this.regs[key] != nil || builtinService(key)
    }

    func createSingletons() void! {
        for key in this.order {
            if this.regs[key].lifetime == Lifetime.Singleton {
                _ = this.rootScope().get(key)?
            }
        }
    }

    // The scope of singletons (scoped services cannot be resolved in it).
    func rootScope() *Scope {
        return new Scope{root: this, items: map[string]any{}, isRoot: true}
    }

    // A new request scope.
    func newScope() *Scope {
        return new Scope{root: this, items: map[string]any{}}
    }

    // A scope outside requests (for jobs and tests); scoped services live
    // as long as it does.
    func CreateScope() *Scope {
        return this.newScope()
    }
}

func newServices() *Services {
    return new Services{regs: map[string]*registration{}, singletons: map[string]any{}}
}

func factoryOf<T>(f func(*Scope) T!) func(*Scope) any! {
    return func(s *Scope) any! {
        var v = f(s)?
        return wrap<T>(v)
    }
}

// Services the container provides without registration.
func builtinService(key string) bool {
    return key == "std.log.Logger" || key == "*std.web.Context" || key == "*std.web.App" || key == "*std.web.Scope" || key == "*std.web.Principal"
}

// An error (`return error(...)` in a function returning `any!` would
// return the error as the value).
func fail(msg string) void! {
    return error(msg)
}

// Scope resolves services; a request has its own (Context.Scope).
struct Scope {
    root *Services
    items map[string]any
    isRoot bool

    func services() *Services {
        return this.root
    }

    func get(key string) any! {
        var svc = this.root
        var reg = svc.regs[key]
        if reg == nil {
            fail("web: no service registered for " + key)?
        }
        if reg.lifetime == Lifetime.Singleton {
            var have = svc.singletons[key]
            if have != nil {
                return have
            }
            var v = reg.factory(svc.rootScope())?
            svc.singletons[key] = v
            return v
        }
        if reg.lifetime == Lifetime.Scoped {
            if this.isRoot {
                fail("web: scoped service " + key + " resolved outside a request scope (from a singleton?)")?
            }
            var have = this.items[key]
            if have != nil {
                return have
            }
            var v = reg.factory(this)?
            this.items[key] = v
            return v
        }
        return reg.factory(this)?
    }

    // Resolves a service of type T.
    func Get<T>() T! {
        return resolve<T>(this)?
    }
}

// Resolves a service of type T in scope s (the builtin services: the
// global std.log.Logger, the current *Context, *App, *Scope and *User).
func resolve<T>(s *Scope) T! {
    var key = keyOf<T>()
    if s.root.regs[key] == nil {
        if std.reflect.TypeOf<T>() == std.reflect.TypeOf<std.log.Logger>() {
            return std.log.Global()
        }
        if std.reflect.TypeOf<T>() == std.reflect.TypeOf<*Scope>() {
            return s
        }
        if std.reflect.TypeOf<T>() == std.reflect.TypeOf<*App>() {
            return s.root.app
        }
        if std.reflect.TypeOf<T>() == std.reflect.TypeOf<*Context>() {
            return Current()
        }
        if std.reflect.TypeOf<T>() == std.reflect.TypeOf<*Principal>() {
            var c = Current()
            if c == nil {
                return nil
            }
            return c.User
        }
    }
    var v = s.get(key)?
    switch v {
        case *box<T> b:
            return b.v
    }
    return error("web: service " + key + " has another type")
}

// Fills the injectable fields of *out: every field with @inject, and for
// a controller (all) every pointer and interface field too.
func injectInto<T>(s *Scope, out *T, all bool) void! {
    for f in std.reflect.Fields<T>() {
        var tag = f.Attribute<inject>()
        if tag == nil || !tag.skip {
            injectField(s, &f.Get(*out), tag != nil, all)?
        }
    }
}

func injectField<F>(s *Scope, out *F, tagged bool, all bool) void! {
    var want = tagged
    if all && (std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Pointer || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Interface) {
        want = true
    }
    if want {
        *out = resolve<F>(s)?
    }
}
