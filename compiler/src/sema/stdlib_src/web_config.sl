namespace std.web.config

// Configuration for std.web applications (FRAMEWORK.md "Конфигурация"):
// layered key/value sources with ASP.NET Core precedence, lowest first:
//
//   appsettings.json, appsettings.yaml (.yml), appsettings.toml
//   appsettings.<Environment>.{json,yaml,yml,toml}
//   environment variables  APP__Section__Key   (prefix configurable)
//   command-line arguments --Section:Key=value, --Section__Key value
//
// Keys are case-insensitive paths "Section:Key" (array elements
// "Hosts:0"). Bind<T> fills a struct by its field names through
// compile-time reflection (nested structs, slices, maps, pointers,
// scalars converted like route parameters) and checks @web.validate;
// Options<T> keeps the bound value and reloads it when a file changes
// (polling sizes and mtimes: CheckForChanges, Watch).

struct LoadOptions {
    // Directory of the appsettings files (default ".").
    Dir string
    // Base name (default "appsettings").
    Name string
    // Environment for appsettings.<env>.* (default $SLANG_ENV, else
    // "production").
    Environment string
    // Prefix of environment variables (default "APP"; "-" disables them).
    EnvPrefix string
    // Command-line arguments (nil: the program's own, std.os.Args()[1..]).
    Args []string
    // Ignore the command line.
    NoArgs bool
    // Do not read appsettings files.
    NoFiles bool
    // A JSON document of defaults below every other source (for example
    // an @embed-ed appsettings.json).
    Defaults string
}

// Configuration is the merged view of the sources.
struct Configuration {
    Environment string
    values map[string]string
    order []string
    args map[string]string
    files []string
    mtimes []int
    sizes []int
    o LoadOptions
    prefix string

    // The value of key ("Section:Key", case-insensitive; "" if unset),
    // arguments and environment variables first.
    func Get(key string) string {
        var (v, ok) = this.lookup(key)
        _ = ok
        return v
    }

    func Has(key string) bool {
        var (v, ok) = this.lookup(key)
        _ = v
        return ok
    }

    func lookup(key string) (string, bool) {
        var k = std.strings.ToLower(key)
        if this.args != nil {
            if this.args[k] != "" || hasKey(this.args, k) {
                return (this.args[k], true)
            }
        }
        if this.prefix != "-" {
            var env = std.os.Getenv(this.prefix + "__" + std.strings.Replace(key, ":", "__"))
            if env != "" {
                return (env, true)
            }
        }
        if hasKey(this.values, k) {
            return (this.values[k], true)
        }
        return ("", false)
    }

    // The keys from files and arguments (lower case), in load order.
    func Keys() []string {
        return this.order
    }

    // The files that were loaded.
    func Files() []string {
        return this.files
    }

    // Names of the direct children of section (lower case, in order).
    func children(section string) []string {
        var p = std.strings.ToLower(section)
        if p != "" {
            p += ":"
        }
        var out []string = []
        for k in this.order {
            if std.strings.HasPrefix(k, p) {
                var rest = k[len(p)..]
                var c = std.strings.Index(rest, ":")
                if c >= 0 {
                    rest = rest[..c]
                }
                var seen = false
                for x in out {
                    if x == rest {
                        seen = true
                    }
                }
                if !seen && rest != "" {
                    out = append(out, rest)
                }
            }
        }
        return out
    }

    func hasSection(section string) bool {
        if this.Has(section) {
            return true
        }
        return len(this.children(section)) > 0
    }

    // Binds section ("" = the root) to a T and validates it.
    func Bind<T>(section string) T! {
        var out T
        var errs []string = []
        bindInto(this, section, &out, &errs)
        if len(errs) > 0 {
            return error("config: " + std.strings.Join(errs, "; "))
        }
        var verrs = std.web.Validate<T>(out)
        if len(verrs) > 0 {
            var msgs []string = []
            for e in verrs {
                msgs = append(msgs, e.Message)
            }
            return error("config: invalid configuration: " + std.strings.Join(msgs, "; "))
        }
        return out
    }

    // Reads the sources again.
    func Reload() void! {
        var fresh = Load(this.o)?
        this.values = fresh.values
        this.order = fresh.order
        this.args = fresh.args
        this.files = fresh.files
        this.mtimes = fresh.mtimes
        this.sizes = fresh.sizes
    }

    // Whether a loaded file changed (or a candidate file appeared or
    // disappeared) since the last load.
    func Changed() bool {
        if this.o.NoFiles {
            return false
        }
        var now = candidates(this.o, this.Environment)
        var present []string = []
        for f in now {
            var st = try std.os.Stat(f) catch err {
                continue
            }
            _ = st
            present = append(present, f)
        }
        if len(present) != len(this.files) {
            return true
        }
        for i, f in this.files {
            if present[i] != f {
                return true
            }
            var st = try std.os.Stat(f) catch err {
                return true
            }
            if st.ModTimeNanos != this.mtimes[i] || st.Size != this.sizes[i] {
                return true
            }
        }
        return false
    }
}

func hasKey(m map[string]string, k string) bool {
    for x in m {
        if x == k {
            return true
        }
    }
    return false
}

func keyJoin(a string, b string) string {
    if a == "" {
        return b
    }
    if b == "" {
        return a
    }
    return a + ":" + b
}

func candidates(o LoadOptions, env string) []string {
    var dir = o.Dir
    if dir == "" {
        dir = "."
    }
    var name = o.Name
    if name == "" {
        name = "appsettings"
    }
    var base = dir + "/" + name
    var out []string = []
    for ext in [".json", ".yaml", ".yml", ".toml"] {
        out = append(out, base + ext)
    }
    for ext in [".json", ".yaml", ".yml", ".toml"] {
        out = append(out, base + "." + env + ext)
    }
    return out
}

// Loads the configuration sources.
func Load(o LoadOptions) *Configuration! {
    var env = o.Environment
    if env == "" {
        env = std.os.Getenv("SLANG_ENV")
    }
    if env == "" {
        env = "production"
    }
    var prefix = o.EnvPrefix
    if prefix == "" {
        prefix = "APP"
    }
    var c = new Configuration{Environment: env, values: map[string]string{}, args: map[string]string{}, o: o, prefix: prefix}
    if o.Defaults != "" {
        var dv = std.encoding.json.ParseValue(o.Defaults)?("config defaults")
        flattenJson(c, "", dv)
    }
    var files []string = []
    if !o.NoFiles {
        files = candidates(o, env)
    }
    for f in files {
        var st = try std.os.Stat(f) catch err {
            continue
        }
        var text = std.io.ReadFile(f)?(f)
        if std.strings.HasSuffix(f, ".json") {
            var v = std.encoding.json.ParseValue(text)?(f)
            flattenJson(c, "", v)
        } else if std.strings.HasSuffix(f, ".toml") {
            var v = std.encoding.toml.ParseValue(text)?(f)
            flattenToml(c, "", v)
        } else {
            var v = std.encoding.yaml.ParseValue(text)?(f)
            flattenYaml(c, "", v)
        }
        c.files = append(c.files, f)
        c.mtimes = append(c.mtimes, st.ModTimeNanos)
        c.sizes = append(c.sizes, st.Size)
    }
    if !o.NoArgs {
        var args = o.Args
        if args == nil {
            var all = std.os.Args()
            if len(all) > 1 {
                args = all[1..]
            }
        }
        parseArgs(c, args)
    }
    return c
}

// Binds the root of the default configuration (Load with the default
// options) to a T.
func Bind<T>() T! {
    var c = Load(LoadOptions{})?
    return c.Bind<T>("")?
}

func set(c *Configuration, key string, value string) {
    var k = std.strings.ToLower(key)
    if !hasKey(c.values, k) {
        c.order = append(c.order, k)
    }
    c.values[k] = value
}

func parseArgs(c *Configuration, args []string) {
    var i = 0
    for i < len(args) {
        var a = args[i]
        i += 1
        if !std.strings.HasPrefix(a, "--") && !std.strings.HasPrefix(a, "/") {
            continue
        }
        var body = std.strings.TrimPrefix(std.strings.TrimPrefix(a, "--"), "/")
        var key = body
        var value = ""
        var eq = std.strings.Index(body, "=")
        if eq >= 0 {
            key = body[..eq]
            value = body[eq + 1..]
        } else if i < len(args) && !std.strings.HasPrefix(args[i], "--") {
            value = args[i]
            i += 1
        }
        key = std.strings.ToLower(std.strings.Replace(key, "__", ":"))
        c.args[key] = value
        if !hasKey(c.values, key) {
            c.order = append(c.order, key)
        }
    }
}

func numText(f float) string {
    if f == std.math.Trunc(f) && std.math.Abs(f) < 1.0e15 {
        return std.fmt.Format("{0}", int(f))
    }
    return std.fmt.Format("{0}", f)
}

func flattenJson(c *Configuration, path string, v std.encoding.json.Value) {
    if v.IsObject() {
        for k in v.Keys() {
            flattenJson(c, keyJoin(path, k), v.Get(k))
        }
    } else if v.IsArray() {
        for i, x in v.Items() {
            flattenJson(c, keyJoin(path, std.fmt.Format("{0}", i)), x)
        }
    } else if v.IsString() {
        set(c, path, try v.String() else "")
    } else if v.IsNumber() {
        set(c, path, numText(try v.Float() else 0.0))
    } else if v.IsBool() {
        if try v.Bool() else false {
            set(c, path, "true")
        } else {
            set(c, path, "false")
        }
    } else {
        set(c, path, "")
    }
}

func flattenYaml(c *Configuration, path string, v std.encoding.yaml.Value) {
    if v.IsMap() {
        for k in v.Keys() {
            flattenYaml(c, keyJoin(path, k), v.Get(k))
        }
    } else if v.IsSeq() {
        for i, x in v.Items() {
            flattenYaml(c, keyJoin(path, std.fmt.Format("{0}", i)), x)
        }
    } else if path != "" {
        set(c, path, v.Text())
    }
}

func flattenToml(c *Configuration, path string, v std.encoding.toml.Value) {
    if v.IsTable() {
        for k in v.Keys() {
            flattenToml(c, keyJoin(path, k), v.Get(k))
        }
    } else if v.IsArray() {
        for i, x in v.Items() {
            flattenToml(c, keyJoin(path, std.fmt.Format("{0}", i)), x)
        }
    } else if v.IsString() {
        set(c, path, try v.String() else "")
    } else if v.IsInt() {
        set(c, path, std.fmt.Format("{0}", try v.Int() else 0))
    } else if v.IsFloat() {
        set(c, path, std.fmt.Format("{0}", try v.Float() else 0.0))
    } else if v.IsBool() {
        if try v.Bool() else false {
            set(c, path, "true")
        } else {
            set(c, path, "false")
        }
    } else if v.IsDateTime() {
        set(c, path, try v.DateTime() else "")
    }
}

// ---- binding ---------------------------------------------------------------------

func bindInto<T>(c *Configuration, key string, out *T, errs *[]string) {
    if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Struct {
        for f in std.reflect.Fields<T>() {
            var name = f.Name
            var tag = f.Attribute<std.reflect.json>()
            if tag != nil && tag.skip {
                continue
            }
            if tag != nil && tag.name != "" {
                name = tag.name
            }
            bindInto(c, keyJoin(key, name), &f.Get(*out), errs)
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Slice {
        bindSlice(c, key, out, errs)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Map {
        if std.reflect.TypeOf<T>().Key.Kind == std.reflect.Kind.String {
            bindMap(c, key, out, errs)
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Pointer {
        if c.hasSection(key) {
            bindPtr(c, key, out, errs)
        }
    } else {
        var (v, ok) = c.lookup(key)
        if ok {
            if !std.web.convert(v, out) {
                *errs = append(*errs, key + ": cannot convert '" + v + "' to " + std.reflect.TypeOf<T>().Name)
            }
        }
    }
}

func bindSlice<E>(c *Configuration, key string, out *[]E, errs *[]string) {
    var n = 0
    for k in c.children(key) {
        var i = try std.strings.ToInt(k) else -1
        if i + 1 > n {
            n = i + 1
        }
    }
    if c.prefix != "-" {
        for n < 1000 && std.os.Getenv(c.prefix + "__" + std.strings.Replace(keyJoin(key, std.fmt.Format("{0}", n)), ":", "__")) != "" {
            n += 1
        }
    }
    if n == 0 {
        // a comma-separated scalar ("a,b")
        var (v, ok) = c.lookup(key)
        if ok && v != "" {
            var res []E = []
            for p in std.strings.Split(v, ",") {
                var e E
                if !std.web.convert(std.strings.Trim(p), &e) {
                    *errs = append(*errs, key + ": cannot convert '" + p + "'")
                }
                res = append(res, e)
            }
            *out = res
        }
        return
    }
    var res []E = []
    for i in 0..n {
        var e E
        bindInto(c, keyJoin(key, std.fmt.Format("{0}", i)), &e, errs)
        res = append(res, e)
    }
    *out = res
}

func bindMap<V>(c *Configuration, key string, out *map[string]V, errs *[]string) {
    var names = c.children(key)
    if len(names) == 0 {
        return
    }
    var m = map[string]V{}
    for k in names {
        var e V
        bindInto(c, keyJoin(key, k), &e, errs)
        m[k] = e
    }
    *out = m
}

func bindPtr<E>(c *Configuration, key string, out **E, errs *[]string) {
    var e E
    bindInto(c, key, &e, errs)
    *out = &e
}

// ---- options with reload ----------------------------------------------------------

// Options<T> holds section bound to T and reloads it: CheckForChanges
// (or a Watch task) re-reads the files when one changed, rebinds and
// revalidates, keeps the previous value if that fails, and calls the
// OnChange listeners with the new one.
struct Options<T> {
    cfg *Configuration
    section string
    value T
    version int
    listeners []func(T)
    lastError string

    // The current value.
    func Value() T {
        return this.value
    }

    // 1 after the first bind, +1 per successful reload.
    func Version() int {
        return this.version
    }

    // The error of the last failed reload ("" if none).
    func LastError() string {
        return this.lastError
    }

    func OnChange(f func(T)) {
        this.listeners = append(this.listeners, f)
    }

    // Reloads now.
    func Reload() void! {
        try {
            this.cfg.Reload()?
            var v = this.cfg.Bind<T>(this.section)?
            this.value = v
            this.version += 1
            this.lastError = ""
        } catch err {
            this.lastError = err.Message()
            return err
        }
        for f in this.listeners {
            f(this.value)
        }
    }

    // Reloads if a configuration file changed; true if it did.
    func CheckForChanges() bool! {
        if !this.cfg.Changed() {
            return false
        }
        this.Reload()?
        return true
    }

    // Polls the files every ms milliseconds until the watcher is stopped.
    func Watch(ms int) *Watcher {
        var w = new Watcher{}
        var o = this
        std.task.Go(func() Task<void> {
            return watchLoop<T>(o, w, ms)
        })
        return w
    }
}

// Stops a Watch.
struct Watcher {
    stopped bool

    func Stop() {
        this.stopped = true
    }
}

async func watchLoop<T>(o *Options<T>, w *Watcher, ms int) {
    for !w.stopped {
        await std.task.Delay(ms)
        if w.stopped {
            break
        }
        _ = try o.CheckForChanges() else false
    }
}

// Binds section of cfg and keeps it for reloading.
func NewOptions<T>(cfg *Configuration, section string) *Options<T>! {
    var v = cfg.Bind<T>(section)?
    return new Options<T>{cfg: cfg, section: section, value: v, version: 1}
}
