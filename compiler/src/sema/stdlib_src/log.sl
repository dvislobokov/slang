// std.log: structured logging in the style of Serilog (docs/FRAMEWORK.md
// section 4). See sema/stdsrc.rs for the rules of these sources.
//
// Message templates are compiled: `logger.Info("user {UserId} paid
// {Amount:0.00}", id, sum)` is checked and split by the compiler
// (sema/logtmpl.rs) and lowered to
//     enabled(logger, Level.Information) &&
//         emit(logger, Level.Information, nil, text, parts, names, formats, values)
// so nothing is evaluated when the level is disabled. `parts` are the
// literal pieces around the holes, `names` the property names, `formats`
// what follows a name (`,10:0.00`), `values` the arguments boxed to `any`
// (`{@X}` arguments destructured through compile-time reflection by
// `destructure`, others converted by `capture`).
//
// Global state (the default logger) lives in the runtime's slot map
// (std.diag.rtSlots, guarded by std.diag.rtLock). Context properties
// (log.Context.Push) are frames of the task context (ABI.md 10.8) with the
// reserved key -101; diag spans use -102.
namespace std.log

// Severity of an event, from the most verbose to the most severe.
enum Level { Verbose, Debug, Information, Warning, Error, Fatal }

// One log event. Properties hold the template properties (in template
// order), then the logger's context and enricher properties; Keys() lists
// them in that order. Message is the rendered template with strings
// written as they are (Serilog's `{Message:lj}`).
struct Event {
    Timestamp  int
    Level      Level
    Template   string
    Message    string
    Properties map[string]any
    Error      error
    Category   string
    keys       []string
    parts      []string
    names      []string
    formats    []string
    values     []any

    // Property names in insertion order.
    func Keys() []string {
        var out []string = []
        for k in this.keys {
            out = append(out, k)
        }
        return out
    }

    // Has reports whether the event has property name.
    func Has(name string) bool {
        for k in this.keys {
            if k == name {
                return true
            }
        }
        return false
    }

    // Get returns property name, or nil.
    func Get(name string) any {
        return this.Properties[name]
    }

    // Sets a property (replacing an existing value).
    func Set(name string, value any) {
        if this.Properties == nil {
            this.Properties = map[string]any{}
        }
        if !this.Has(name) {
            this.keys = append(this.keys, name)
        }
        this.Properties[name] = value
    }

    // Adds a property unless it is already set.
    func AddIfAbsent(name string, value any) {
        if !this.Has(name) {
            this.Set(name, value)
        }
    }

    // The message rendered with strings quoted (Serilog's default
    // `{Message}`) or as they are (literal).
    func Render(literal bool) string {
        if len(this.parts) == 0 {
            return this.Message
        }
        var b = std.strings.Builder{}
        for i, p in this.parts {
            b.Write(p)
            if i < len(this.names) {
                b.Write(
                    applyAlign(
                        renderValue(this.values[i], formatOf(this.formats[i]), !literal),
                        this.formats[i],
                    ),
                )
            }
        }
        return b.String()
    }
}

// A destructured struct (`{@Order}`): Serilog's StructureValue.
struct Structure {
    TypeTag string
    Names   []string
    Values  []any

    // Get returns the value of field name, or nil.
    func Get(name string) any {
        for i, n in this.Names {
            if n == name {
                return this.Values[i]
            }
        }
        return nil
    }
}

// A destructured map with string keys (keys sorted).
struct Dictionary {
    Keys   []string
    Values []any
}

// The logger interface (inject it; `log.For<T>()` gives one with a
// category). `logger.Info("...", args)` and the other level methods are
// compiled templates available on every Logger.
interface Logger {
    // IsEnabled reports whether events of level would be written.
    func IsEnabled(level std.log.Level) bool
    // Write sends a complete event through the logger's enrichers, filters and sinks.
    func Write(e *std.log.Event)
    // ForContext returns a logger that adds property name = value to every event.
    func ForContext(name string, value any) std.log.Logger
    // ForCategory returns a logger whose events have the SourceContext category.
    func ForCategory(category string) std.log.Logger
    // Close flushes and closes the logger's sinks.
    func Close() Task<void>
}

// A destination of events.
interface Sink {
    // Emit writes one event.
    func Emit(e *std.log.Event)
    // Close flushes buffered events and releases the sink.
    func Close() Task<void>
}

// Adds properties to events.
interface Enricher {
    // Enrich adds or changes properties of e.
    func Enrich(e *std.log.Event)
}

// A minimum level that can be changed while the program runs
// (MinimumLevelControlledBy).
struct LevelSwitch {
    MinimumLevel Level

    // Set changes the minimum level.
    func Set(level Level) {
        this.MinimumLevel = level
    }
}

// NewLevelSwitch returns a switch starting at level (pass it to MinimumLevel.ControlledBy).
func NewLevelSwitch(level Level) *LevelSwitch {
    return new LevelSwitch{MinimumLevel: level}
}

struct override {
    prefix string
    level  Level
}

struct prop {
    name  string
    value any
}

// ---- configuration --------------------------------------------------------

struct cfgState {
    min       Level
    sw        *LevelSwitch
    overrides []override
    enrichers []Enricher
    sinks     []Sink
    filters   []func(*Event) bool
    clock     func() int
}

// Configuration builds a logger: log.New().MinimumLevel(...).Enrich....
// .WriteTo....CreateLogger(). Copies share their state.
struct Configuration {
    st      *cfgState
    Enrich  EnrichConfiguration
    WriteTo SinkConfiguration
    Filter  FilterConfiguration

    // MinimumLevel sets the minimum level of the logger (default Information).
    func MinimumLevel(level Level) Configuration {
        this.st.min = level
        return wrap(this.st)
    }

    // Events whose category is `prefix` or starts with `prefix.` use level.
    func MinimumLevelOverride(prefix string, level Level) Configuration {
        this.st.overrides = append(this.st.overrides, override{prefix: prefix, level: level})
        return wrap(this.st)
    }

    // The minimum level follows sw (changeable at run time).
    func MinimumLevelControlledBy(sw *LevelSwitch) Configuration {
        this.st.sw = sw
        return wrap(this.st)
    }

    // Timestamps come from f (Unix milliseconds) instead of the clock:
    // deterministic output in tests.
    func Clock(f func() int) Configuration {
        this.st.clock = f
        return wrap(this.st)
    }

    // CreateLogger builds the logger.
    func CreateLogger() Logger {
        var st = this.st
        var p = new Pipeline{min: st.min, sw: st.sw, clock: st.clock}
        p.overrides = copyOverrides(st.overrides)
        for e in st.enrichers {
            p.enrichers = append(p.enrichers, e)
        }
        for s in st.sinks {
            p.sinks = append(p.sinks, s)
        }
        for f in st.filters {
            p.filters = append(p.filters, f)
        }
        return p
    }
}

func wrap(st *cfgState) Configuration {
    return Configuration{
        st: st,
        Enrich: EnrichConfiguration{st: st},
        WriteTo: SinkConfiguration{st: st},
        Filter: FilterConfiguration{st: st},
    }
}

func copyOverrides(o []override) []override {
    var out []override = []
    for x in o {
        out = append(out, x)
    }
    return out
}

// A new configuration (minimum level Information, no sinks).
func New() Configuration {
    return wrap(new cfgState{min: Level.Information})
}

// EnrichConfiguration adds enrichers (Configuration.Enrich).
struct EnrichConfiguration {
    st *cfgState

    // WithProperty adds property name = value to every event.
    func WithProperty(name string, value any) Configuration {
        return this.With(new propertyEnricher{name: name, value: value})
    }

    // MachineName = the host name.
    func WithMachineName() Configuration {
        var h = try std.os.Hostname() else "unknown"
        return this.With(new propertyEnricher{name: "MachineName", value: h})
    }

    // ProcessId = the process id.
    func WithProcessId() Configuration {
        return this.With(new propertyEnricher{name: "ProcessId", value: std.os.Pid()})
    }

    // Properties pushed with log.Context.Push, and TraceId / SpanId of the
    // current std.diag span.
    func FromContext() Configuration {
        return this.With(new contextEnricher{})
    }

    // With adds a custom enricher.
    func With(e Enricher) Configuration {
        this.st.enrichers = append(this.st.enrichers, e)
        return wrap(this.st)
    }

    // An enricher from a function.
    func WithFunc(f func(*Event)) Configuration {
        return this.With(new funcEnricher{f: f})
    }
}

// SinkConfiguration adds sinks (Configuration.WriteTo).
struct SinkConfiguration {
    st *cfgState

    // Text lines on stderr (or stdout with Stdout: true).
    func Console(opts ConsoleOptions) Configuration {
        return this.Sink(NewConsoleSink(opts))
    }

    // Text (or JSON) lines appended to a file, optionally rolled by day or
    // size.
    func File(path string, opts FileOptions) Configuration {
        return this.Sink(NewFileSink(path, opts))
    }

    // Compact JSON lines (CLEF: @t, @mt, @m, @l, @x, properties) on stdout,
    // stderr or a file.
    func Json(opts JsonOptions) Configuration {
        return this.Sink(NewJsonSink(opts))
    }

    // Sink adds a custom sink.
    func Sink(s Sink) Configuration {
        this.st.sinks = append(this.st.sinks, s)
        return wrap(this.st)
    }

    // s behind a queue of `capacity` events drained by a background task.
    func Async(s Sink, capacity int) Configuration {
        return this.Sink(NewAsyncSink(s, capacity))
    }
}

// FilterConfiguration adds filters (Configuration.Filter).
struct FilterConfiguration {
    st *cfgState

    // Drops events for which f is true.
    func ByExcluding(f func(*Event) bool) Configuration {
        this.st.filters = append(this.st.filters, f)
        return wrap(this.st)
    }

    // Keeps only events for which f is true.
    func ByIncludingOnly(f func(*Event) bool) Configuration {
        this.st.filters = append(this.st.filters, func(e *Event) bool {
            return !f(e)
        })
        return wrap(this.st)
    }
}

// ---- the logger -----------------------------------------------------------

// Pipeline is the Logger made by CreateLogger.
struct Pipeline {
    min       Level
    sw        *LevelSwitch
    fixed     bool
    overrides []override
    enrichers []Enricher
    sinks     []Sink
    filters   []func(*Event) bool
    clock     func() int
    category  string
    props     []prop

    // IsEnabled reports whether events of level pass the minimum level (or the level switch).
    func IsEnabled(level Level) bool {
        var m = this.min
        if this.sw != nil && !this.fixed {
            m = this.sw.MinimumLevel
        }
        return int(level) >= int(m)
    }

    // Write timestamps, enriches and filters e and emits it to every sink.
    func Write(e *Event) {
        if this.clock != nil {
            e.Timestamp = this.clock()
        } else if e.Timestamp == 0 {
            e.Timestamp = std.time.Now()
        }
        if e.Category == "" {
            e.Category = this.category
        }
        if e.Category != "" {
            e.AddIfAbsent("SourceContext", e.Category)
        }
        for p in this.props {
            e.AddIfAbsent(p.name, p.value)
        }
        for en in this.enrichers {
            en.Enrich(e)
        }
        for f in this.filters {
            if f(e) {
                return
            }
        }
        for s in this.sinks {
            s.Emit(e)
        }
    }

    func derive() *Pipeline {
        var p = new Pipeline{
            min: this.min,
            sw: this.sw,
            fixed: this.fixed,
            overrides: this.overrides,
            enrichers: this.enrichers,
            sinks: this.sinks,
            filters: this.filters,
            clock: this.clock,
            category: this.category,
        }
        for x in this.props {
            p.props = append(p.props, x)
        }
        return p
    }

    // ForContext returns a logger that adds property name = value to every event.
    func ForContext(name string, value any) Logger {
        var p = this.derive()
        p.props = append(p.props, prop{name: name, value: value})
        return p
    }

    // A logger for a category (SourceContext); MinimumLevelOverride
    // entries apply by longest prefix.
    func ForCategory(category string) Logger {
        var p = this.derive()
        p.category = category
        var best = -1
        for o in this.overrides {
            if (category == o.prefix || std.strings.HasPrefix(category, o.prefix + ".")) &&
                len(o.prefix) > best {
                best = len(o.prefix)
                p.min = o.level
                p.fixed = true
            }
        }
        return p
    }

    // Flushes and closes the sinks.
    func Close() Task<void> {
        return closeSinks(this.sinks)
    }
}

async func closeSinks(sinks []Sink) {
    for s in sinks {
        await s.Close()
    }
}

// ---- the default logger ---------------------------------------------------

struct globalHolder {
    l Logger
}

// The default logger: Information and above as "LEVEL message" lines on
// stderr until SetGlobal replaces it.
func Global() Logger {
    std.diag.rtLock()
    var m = std.diag.rtSlots()
    if m == nil {
        m = map[string]any{}
        std.diag.rtSetSlots(m)
    }
    switch m["log.global"] {
        case globalHolder h:
            std.diag.rtUnlock()
            return h.l
    }
    var l = New().WriteTo.Console(ConsoleOptions{
        Template: "{Level:u4} {Message:lj}{NewLine}{Exception}",
    }).CreateLogger()
    m["log.global"] = globalHolder{l: l}
    std.diag.rtUnlock()
    return l
}

// Replaces the default logger (used by log.Info(...) and log.For<T>()).
func SetGlobal(l Logger) {
    std.diag.rtLock()
    var m = std.diag.rtSlots()
    if m == nil {
        m = map[string]any{}
        std.diag.rtSetSlots(m)
    }
    m["log.global"] = globalHolder{l: l}
    std.diag.rtUnlock()
}

// Flushes and closes the default logger's sinks.
func CloseAndFlush() Task<void> {
    return Global().Close()
}

// A logger whose category is the full name of T.
func For<T>() Logger {
    return Global().ForCategory(std.reflect.TypeOf<T>().FullName)
}

// ForContext of the default logger.
func ForContext(name string, value any) Logger {
    return Global().ForContext(name, value)
}

// The package-level level functions log through Global(). Calls are
// compiled templates like the Logger methods; these bodies only serve
// uses as function values (the text is the message).
func Verbose(message string) {
    write(Global(), Level.Verbose, message)
}

// Debug logs a message template at Debug level through the default logger.
func Debug(message string) {
    write(Global(), Level.Debug, message)
}

// Information logs a message template at Information level through the default logger.
func Information(message string) {
    write(Global(), Level.Information, message)
}

// Info is Information.
func Info(message string) {
    write(Global(), Level.Information, message)
}

// Warning logs a message template at Warning level through the default logger.
func Warning(message string) {
    write(Global(), Level.Warning, message)
}

// Warn is Warning.
func Warn(message string) {
    write(Global(), Level.Warning, message)
}

// Error logs a message template at Error level through the default logger.
func Error(message string) {
    write(Global(), Level.Error, message)
}

// Fatal logs a message template at Fatal level through the default logger.
func Fatal(message string) {
    write(Global(), Level.Fatal, message)
}

func write(l Logger, level Level, message string) {
    if l.IsEnabled(level) {
        l.Write(new Event{Level: level, Template: message, Message: message})
    }
}

// ---- compiled templates (targets of sema/logtmpl.rs) ----------------------

func enabled(l Logger, level Level) bool {
    if l == nil {
        return Global().IsEnabled(level)
    }
    return l.IsEnabled(level)
}

func emit(
    l Logger,
    level Level,
    err error,
    text string,
    parts []string,
    names []string,
    formats []string,
    values []any,
) bool {
    var lg = l
    if lg == nil {
        lg = Global()
    }
    var e = new Event{
        Level: level,
        Template: text,
        Error: err,
        parts: parts,
        names: names,
        formats: formats,
        values: values,
    }
    e.Properties = map[string]any{}
    for i, n in names {
        e.AddIfAbsent(n, values[i])
    }
    if err != nil {
        e.Set("ErrorMessage", err.Message())
        var tr = std.errors.Trace(err)
        if tr != "" {
            e.Set("ErrorTrace", tr)
        }
    }
    e.Message = e.Render(true)
    lg.Write(e)
    return true
}

// The scalar value of a template argument (no destructuring): numbers,
// strings, bools, errors and `any` as they are, chars as strings, enums by
// variant name, slices and arrays element-wise, pointers by their target,
// other types (structs, maps, functions) by their type name.
func capture<T>(v T) any {
    if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.String ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Bool ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Any ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Error {
        return v
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int8 ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int16 ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Int32 ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint8 ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint16 ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Uint32 {
        return int(v)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Float32 {
        return float(v)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Char {
        return string(v)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Enum {
        return std.fmt.Format("{0}", v)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Slice ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Array {
        var xs []any = []
        for x in v {
            xs = append(xs, capture(x))
        }
        return xs
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Pointer {
        if v == nil {
            return nil
        }
        return capture(*v)
    } else {
        return std.reflect.TypeOf<T>().Name
    }
}

// The destructured value of a `{@X}` argument: structs become a
// Structure of their exported fields (fields with @log(skip: true) left
// out), maps with string keys a Dictionary, slices element-wise; scalars
// as capture. Nesting deeper than 10 levels is cut off.
func destructure<T>(v T) any {
    return destructureAt(v, 0)
}

func destructureAt<T>(v T, depth int) any {
    if depth > 10 {
        return "..."
    }
    if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Struct {
        var names []string = []
        var values []any = []
        for f in std.reflect.Fields<T>() {
            var tag = f.Attribute<std.reflect.logAttr>()
            if tag != nil && tag.skip {
                continue
            }
            if !f.Exported {
                continue
            }
            names = append(names, f.Name)
            values = append(values, destructureAt(f.Get(v), depth + 1))
        }
        return Structure{TypeTag: std.reflect.TypeOf<T>().Name, Names: names, Values: values}
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Pointer {
        if v == nil {
            return nil
        }
        return destructureAt(*v, depth)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Slice ||
        std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Array {
        var xs []any = []
        for x in v {
            xs = append(xs, destructureAt(x, depth + 1))
        }
        return xs
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Map {
        if std.reflect.TypeOf<T>().Key.Kind == std.reflect.Kind.String {
            var keys []string = []
            for k, _ in v {
                keys = append(keys, k)
            }
            sortStrings(keys)
            var vals []any = []
            for k in keys {
                vals = append(vals, destructureAt(v[k], depth + 1))
            }
            return Dictionary{Keys: keys, Values: vals}
        } else {
            return std.reflect.TypeOf<T>().Name
        }
    } else {
        return capture(v)
    }
}

func sortStrings(s []string) {
    for i in 1..len(s) {
        var j = i
        for j > 0 && s[j - 1] > s[j] {
            var t = s[j]
            s[j] = s[j - 1]
            s[j - 1] = t
            j -= 1
        }
    }
}

// ---- context properties ---------------------------------------------------

// log.Context.Push(name, value) adds a property to every event logged in
// the current task (and in tasks it starts) until the returned scope is
// popped: `var s = log.Context.Push("RequestId", id); defer s.Pop()`.
struct Context {
    // Push adds property name = value to the events of the current task until the scope is popped.
    static func Push(name string, value any) ContextScope {
        var old = std.task.rtGetCtx()
        var c = std.task.CurrentContext()
        var fr = new std.task.ctxFrame{
            key: -101,
            val: new prop{name: name, value: value},
            next: c.top(),
        }
        var m = map[string]any{}
        m[""] = fr
        std.task.rtSetCtx(m)
        return ContextScope{old: old}
    }

    // The pushed properties, innermost first.
    static func Properties() []prop {
        var out []prop = []
        var c = std.task.CurrentContext()
        var f = c.top()
        for f != nil {
            if f.key == -101 {
                switch f.val {
                    case *prop p:
                        out = append(out, *p)
                }
            }
            f = f.next
        }
        return out
    }
}

// Restores the context of before a Context.Push.
struct ContextScope {
    old map[string]any

    // Pop removes the property (restores the context of before Push).
    func Pop() {
        std.task.rtSetCtx(this.old)
    }
}

struct propertyEnricher {
    name  string
    value any

    func Enrich(e *Event) {
        e.AddIfAbsent(this.name, this.value)
    }
}

struct contextEnricher {
    func Enrich(e *Event) {
        for p in Context.Properties() {
            e.AddIfAbsent(p.name, p.value)
        }
        var sp = std.diag.CurrentSpan()
        if sp != nil {
            e.AddIfAbsent("TraceId", sp.TraceId)
            e.AddIfAbsent("SpanId", sp.SpanId)
        }
    }
}

struct funcEnricher {
    f func(*Event)

    func Enrich(e *Event) {
        this.f(e)
    }
}

// ---- rendering ------------------------------------------------------------

// `,10:0.00` -> `0.00`.
func formatOf(spec string) string {
    var i = std.strings.Index(spec, ":")
    if i < 0 {
        return ""
    }
    return spec[i + 1..]
}

// Applies the `,N` (right-aligned) / `,-N` (left-aligned) part of spec.
func applyAlign(s string, spec string) string {
    if !std.strings.HasPrefix(spec, ",") {
        return s
    }
    var w = spec[1..]
    var i = std.strings.Index(w, ":")
    if i >= 0 {
        w = w[..i]
    }
    var n = try std.strings.ToInt(w) else 0
    if n < 0 {
        return std.strings.PadRight(s, -n, " ")
    }
    return std.strings.PadLeft(s, n, " ")
}

func quote(s string) string {
    var b = std.strings.Builder{}
    b.Write("\"")
    var start = 0
    for i in 0..len(s) {
        var c = s[i..i + 1]
        var esc = ""
        if c == "\"" {
            esc = "\\\""
        } else if c == "\\" {
            esc = "\\\\"
        } else if c == "\n" {
            esc = "\\n"
        } else if c == "\r" {
            esc = "\\r"
        } else if c == "\t" {
            esc = "\\t"
        } else if s[i] < 32 {
            esc = std.fmt.Sprintf("\\u%04x", int(s[i]))
        }
        if esc != "" {
            b.Write(s[start..i])
            b.Write(esc)
            start = i + 1
        }
    }
    b.Write(s[start..])
    b.Write("\"")
    return b.String()
}

func fmtInt(n int, f string) string {
    if f == "" {
        return std.fmt.Format("{0}", n)
    }
    var c = f[0..1]
    var rest = f[1..]
    if c == "x" || c == "X" {
        var s = std.fmt.Sprintf("%" + "x", n)
        if c == "X" {
            s = std.strings.ToUpper(s)
        }
        if rest != "" {
            var w = try std.strings.ToInt(rest) else 0
            s = std.strings.PadLeft(s, w, "0")
        }
        return s
    }
    if c == "D" || c == "d" {
        var w = try std.strings.ToInt(rest) else 0
        if n < 0 {
            return "-" + std.strings.PadLeft(std.fmt.Format("{0}", -n), w, "0")
        }
        return std.strings.PadLeft(std.fmt.Format("{0}", n), w, "0")
    }
    return fmtFloat(float(n), f)
}

// Number formats: "0.00" (fixed, as many decimals as zeros after the
// point), "F2" / "N2" (fixed), "E2" (exponent), "P1" (percent), "0"
// (rounded); anything else is ignored.
func fmtFloat(x float, f string) string {
    if f == "" {
        return std.fmt.Format("{0}", x)
    }
    var c = f[0..1]
    if c == "0" || c == "#" {
        var dot = std.strings.Index(f, ".")
        var d = 0
        if dot >= 0 {
            d = len(f) - dot - 1
        }
        return std.fmt.Sprintf("%." + std.fmt.Format("{0}", d) + "f", x)
    }
    var d = 2
    if len(f) > 1 {
        d = try std.strings.ToInt(f[1..]) else 2
    }
    if c == "F" || c == "f" || c == "N" || c == "n" {
        return std.fmt.Sprintf("%." + std.fmt.Format("{0}", d) + "f", x)
    }
    if c == "E" || c == "e" {
        return std.fmt.Sprintf("%." + std.fmt.Format("{0}", d) + "e", x)
    }
    if c == "P" || c == "p" {
        return std.fmt.Sprintf("%." + std.fmt.Format("{0}", d) + "f", x * 100.0) + "%"
    }
    return std.fmt.Format("{0}", x)
}

// Text of a property value: strings quoted when `quoted` (unless the
// format is "l"), numbers with the format, structures as
// `Type { A: 1, B: "x" }`, sequences as `[1, 2]`, nil as `null`.
func renderValue(v any, f string, quoted bool) string {
    switch v {
        case string s:
            if quoted && f != "l" {
                return quote(s)
            }
            return s
        case int n:
            return fmtInt(n, f)
        case float x:
            return fmtFloat(x, f)
        case bool t:
            if t {
                return "true"
            }
            return "false"
        case error e:
            return e.Message()
        case Structure st:
            var b = std.strings.Builder{}
            if st.TypeTag != "" {
                b.Write(st.TypeTag)
                b.Write(" ")
            }
            b.Write("{")
            for i, n in st.Names {
                if i > 0 {
                    b.Write(",")
                }
                b.Write(" ")
                b.Write(n)
                b.Write(": ")
                b.Write(renderValue(st.Values[i], "", true))
            }
            b.Write(" }")
            return b.String()
        case Dictionary d:
            var b = std.strings.Builder{}
            b.Write("[")
            for i, k in d.Keys {
                if i > 0 {
                    b.Write(", ")
                }
                b.Write("(" + quote(k) + ": " + renderValue(d.Values[i], "", true) + ")")
            }
            b.Write("]")
            return b.String()
        case []any xs:
            var b = std.strings.Builder{}
            b.Write("[")
            for i, x in xs {
                if i > 0 {
                    b.Write(", ")
                }
                b.Write(renderValue(x, f, true))
            }
            b.Write("]")
            return b.String()
    }
    if v == nil {
        return "null"
    }
    return std.fmt.Format("{0}", v)
}

// JSON text of a property value.
func jsonValue(v any) string {
    switch v {
        case string s:
            return quote(s)
        case int n:
            return std.fmt.Format("{0}", n)
        case float x:
            if std.math.IsNaN(x) || std.math.IsInf(x, 0) {
                return quote(std.fmt.Format("{0}", x))
            }
            return std.fmt.Format("{0}", x)
        case bool t:
            if t {
                return "true"
            }
            return "false"
        case error e:
            return quote(e.Message())
        case Structure st:
            var b = std.strings.Builder{}
            b.Write("{")
            var first = true
            if st.TypeTag != "" {
                b.Write("\"$type\":" + quote(st.TypeTag))
                first = false
            }
            for i, n in st.Names {
                if !first {
                    b.Write(",")
                }
                first = false
                b.Write(quote(n) + ":" + jsonValue(st.Values[i]))
            }
            b.Write("}")
            return b.String()
        case Dictionary d:
            var b = std.strings.Builder{}
            b.Write("{")
            for i, k in d.Keys {
                if i > 0 {
                    b.Write(",")
                }
                b.Write(quote(k) + ":" + jsonValue(d.Values[i]))
            }
            b.Write("}")
            return b.String()
        case []any xs:
            var b = std.strings.Builder{}
            b.Write("[")
            for i, x in xs {
                if i > 0 {
                    b.Write(",")
                }
                b.Write(jsonValue(x))
            }
            b.Write("]")
            return b.String()
    }
    if v == nil {
        return "null"
    }
    return quote(std.fmt.Format("{0}", v))
}

func levelName(l Level) string {
    switch l {
        case Level.Verbose:
            return "Verbose"
        case Level.Debug:
            return "Debug"
        case Level.Information:
            return "Information"
        case Level.Warning:
            return "Warning"
        case Level.Error:
            return "Error"
    }
    return "Fatal"
}

// Serilog's level formats: u3 (INF), w3 (inf), u4 (INFO), w4, t3 (Inf).
func levelText(l Level, f string) string {
    var i = int(l)
    if f == "u3" || f == "w3" || f == "t3" {
        var s = ["VRB", "DBG", "INF", "WRN", "ERR", "FTL"][i]
        if f == "w3" {
            return std.strings.ToLower(s)
        }
        if f == "t3" {
            return s[0..1] + std.strings.ToLower(s[1..])
        }
        return s
    }
    if f == "u4" || f == "w4" {
        var s = ["VERB", "DBUG", "INFO", "WARN", "EROR", "FATL"][i]
        if f == "w4" {
            return std.strings.ToLower(s)
        }
        return s
    }
    if f == "u" {
        return std.strings.ToUpper(levelName(l))
    }
    if f == "w" {
        return std.strings.ToLower(levelName(l))
    }
    return levelName(l)
}

// .NET-style date format -> strftime (std.time.Format): yyyy yy MM dd HH
// hh mm ss fff tt zzz; other letters in quotes are literal.
func timeLayout(f string) string {
    var b = std.strings.Builder{}
    var i = 0
    for i < len(f) {
        var c = f[i..i + 1]
        var n = 1
        for i + n < len(f) && f[i + n..i + n + 1] == c {
            n += 1
        }
        var tok = f[i..i + n]
        if tok == "yyyy" {
            b.Write("%Y")
        } else if tok == "yy" {
            b.Write("%y")
        } else if tok == "MM" {
            b.Write("%m")
        } else if tok == "dd" {
            b.Write("%d")
        } else if tok == "HH" {
            b.Write("%H")
        } else if tok == "hh" {
            b.Write("%I")
        } else if tok == "mm" {
            b.Write("%M")
        } else if tok == "ss" {
            b.Write("%S")
        } else if tok == "fff" {
            b.Write("%L")
        } else if tok == "tt" {
            b.Write("%p")
        } else if tok == "zzz" {
            b.Write("+00:00")
        } else if c == "%" {
            b.Write(std.strings.Repeat("%%", n))
        } else {
            b.Write(tok)
        }
        i += n
    }
    return b.String()
}

func formatTime(ms int, f string) string {
    var layout = f
    if layout == "" {
        layout = "yyyy-MM-dd HH:mm:ss.fff zzz"
    }
    return std.time.Format(ms, timeLayout(layout))
}

// ---- output templates -----------------------------------------------------

struct segment {
    text   string
    name   string
    format string
    align  string
}

// Splits an output template ("{Timestamp:HH:mm:ss} [{Level:u3}] {Message:lj}{NewLine}").
func parseOutput(t string) []segment {
    var out []segment = []
    var lit = ""
    var i = 0
    for i < len(t) {
        var c = t[i..i + 1]
        if c == "{" && i + 1 < len(t) && t[i + 1..i + 2] == "{" {
            lit += "{"
            i += 2
            continue
        }
        if c == "}" && i + 1 < len(t) && t[i + 1..i + 2] == "}" {
            lit += "}"
            i += 2
            continue
        }
        if c == "{" {
            var end = std.strings.Index(t[i..], "}")
            if end < 0 {
                lit += t[i..]
                break
            }
            if lit != "" {
                out = append(out, segment{text: lit})
                lit = ""
            }
            var hole = t[i + 1..i + end]
            var name = hole
            var format = ""
            var align = ""
            var colon = std.strings.Index(hole, ":")
            if colon >= 0 {
                name = hole[..colon]
                format = hole[colon + 1..]
            }
            var comma = std.strings.Index(name, ",")
            if comma >= 0 {
                align = "," + name[comma + 1..]
                name = name[..comma]
            }
            out = append(out, segment{name: name, format: format, align: align})
            i += end + 1
            continue
        }
        lit += c
        i += 1
    }
    if lit != "" {
        out = append(out, segment{text: lit})
    }
    return out
}

func levelColor(l Level) string {
    switch l {
        case Level.Verbose:
            return "\u{1b}[90m"
        case Level.Debug:
            return "\u{1b}[90m"
        case Level.Information:
            return "\u{1b}[36m"
        case Level.Warning:
            return "\u{1b}[33m"
        case Level.Error:
            return "\u{1b}[31m"
    }
    return "\u{1b}[97;41m"
}

func exceptionText(e *Event) string {
    if e.Error == nil {
        return ""
    }
    var s = e.Error.Message() + "\n"
    var tr = std.errors.Trace(e.Error)
    if tr != "" {
        s += tr
        if !std.strings.HasSuffix(tr, "\n") {
            s += "\n"
        }
    }
    return s
}

func propertiesText(e *Event, f string) string {
    var b = std.strings.Builder{}
    b.Write("{")
    for i, k in e.keys {
        if i > 0 {
            b.Write(",")
        }
        b.Write(" " + k + ": " + renderValue(e.Properties[k], "", true))
    }
    b.Write(" }")
    if f == "j" {
        var j = std.strings.Builder{}
        j.Write("{")
        for i, k in e.keys {
            if i > 0 {
                j.Write(",")
            }
            j.Write(quote(k) + ":" + jsonValue(e.Properties[k]))
        }
        j.Write("}")
        return j.String()
    }
    return b.String()
}

// Renders e with an output template.
func renderOutput(segs []segment, e *Event, colors bool) string {
    var b = std.strings.Builder{}
    for s in segs {
        if s.name == "" {
            b.Write(s.text)
            continue
        }
        var v = ""
        if s.name == "Timestamp" {
            v = formatTime(e.Timestamp, s.format)
        } else if s.name == "Level" {
            v = levelText(e.Level, s.format)
            if colors {
                v = levelColor(e.Level) + v + "\u{1b}[0m"
            }
        } else if s.name == "Message" {
            if s.format == "lj" || s.format == "l" {
                v = e.Message
            } else {
                v = e.Render(false)
            }
        } else if s.name == "NewLine" {
            v = "\n"
        } else if s.name == "Exception" {
            v = exceptionText(e)
        } else if s.name == "Properties" {
            v = propertiesText(e, s.format)
        } else if s.name == "Category" || (s.name == "SourceContext" && !e.Has("SourceContext")) {
            v = e.Category
        } else if e.Has(s.name) {
            v = renderValue(e.Properties[s.name], s.format, s.format != "l" && s.format != "lj")
        }
        b.Write(applyAlign(v, s.align))
    }
    return b.String()
}

// One compact JSON (CLEF) line.
func clef(e *Event) string {
    var b = std.strings.Builder{}
    b.Write("{\"@t\":" + quote(formatTime(e.Timestamp, "yyyy-MM-ddTHH:mm:ss.fffZ")))
    b.Write(",\"@mt\":" + quote(e.Template))
    b.Write(",\"@m\":" + quote(e.Message))
    if e.Level != Level.Information {
        b.Write(",\"@l\":" + quote(levelName(e.Level)))
    }
    if e.Error != nil {
        b.Write(",\"@x\":" + quote(exceptionText(e)))
    }
    for k in e.keys {
        if k == "ErrorMessage" || k == "ErrorTrace" {
            continue
        }
        var name = k
        if std.strings.HasPrefix(k, "@") {
            name = "@" + k
        }
        b.Write("," + quote(name) + ":" + jsonValue(e.Properties[k]))
    }
    b.Write("}\n")
    return b.String()
}

// ---- sinks ----------------------------------------------------------------

// ConsoleOptions configures NewConsoleSink.
struct ConsoleOptions {
    // Output template; "" = "[{Timestamp:HH:mm:ss} {Level:u3}] {Message:lj}{NewLine}{Exception}".
    Template string
    // Write to stdout instead of stderr.
    Stdout bool
    // ANSI colors for the level.
    Colors bool
}

// ConsoleSink writes rendered text lines to stderr or stdout.
struct ConsoleSink {
    segs   []segment
    stdout bool
    colors bool

    // Emit renders e with the output template and writes it.
    func Emit(e *Event) {
        var s = renderOutput(this.segs, e, this.colors)
        if this.stdout {
            std.io.print(s)
        } else {
            std.io.Eprint(s)
        }
    }

    // Close does nothing (the console is not buffered).
    func Close() Task<void> {
        return std.task.Delay(0)
    }
}

// NewConsoleSink returns a console sink with opts (the default template when Template is "").
func NewConsoleSink(opts ConsoleOptions) *ConsoleSink {
    var t = opts.Template
    if t == "" {
        t = "[{Timestamp:HH:mm:ss} {Level:u3}] {Message:lj}{NewLine}{Exception}"
    }
    return new ConsoleSink{segs: parseOutput(t), stdout: opts.Stdout, colors: opts.Colors}
}

// How a file sink starts new files.
enum RollingInterval { Infinite, Day, Hour, Month, Year }

// FileOptions configures NewFileSink: template or JSON, rolling and size limits.
struct FileOptions {
    // Output template ("" = "{Timestamp:yyyy-MM-dd HH:mm:ss.fff zzz} [{Level:u3}] {Message:lj}{NewLine}{Exception}").
    Template string
    // CLEF JSON lines instead of text.
    Json bool
    // A new file per period: "logs/app-.log" -> "logs/app-20240131.log".
    RollingInterval RollingInterval
    // Start a new file once this many bytes are written (0 = no limit):
    // "app-20240131_001.log", ...
    FileSizeLimit int
    // Keep at most this many files of the sink (0 = all).
    Retain int
}

// FileSink appends events to a file, rolling to new files by interval or size.
struct FileSink {
    guard   int
    base    string
    opts    FileOptions
    segs    []segment
    fd      int
    period  string
    seq     int
    size    int
    current string
    err     string

    func pathFor(period string, seq int) string {
        var dir = std.fs.Dir(this.base)
        var name = std.fs.Base(this.base)
        var ext = std.fs.Ext(name)
        var stem = name[..len(name) - len(ext)]
        var suffix = period
        if seq > 0 {
            suffix += "_" + std.strings.PadLeft(std.fmt.Format("{0}", seq), 3, "0")
        }
        return std.fs.Join(dir, stem + suffix + ext)
    }

    func open(period string) {
        if this.fd >= 0 {
            rtClose(this.fd)
            this.fd = -1
        }
        var dir = std.fs.Dir(this.base)
        if dir != "" && dir != "." {
            _ = std.fs.MkdirAll(dir)
        }
        var p = this.pathFor(period, this.seq)
        this.fd = try rtOpen(p) catch err {
            this.err = err.Message()
            return
        }
        this.current = p
        this.period = period
        this.size = try std.fs.Size(p) else 0
        this.retain()
    }

    // Deletes the oldest files of this sink beyond opts.Retain.
    func retain() {
        if this.opts.Retain <= 0 {
            return
        }
        var dir = std.fs.Dir(this.base)
        var name = std.fs.Base(this.base)
        var ext = std.fs.Ext(name)
        var stem = name[..len(name) - len(ext)]
        var entries = try std.fs.ReadDir(dir) else []
        var mine []string = []
        for n in entries {
            var b = std.fs.Base(n)
            if std.strings.HasPrefix(b, stem) && std.strings.HasSuffix(b, ext) {
                mine = append(mine, b)
            }
        }
        sortStrings(mine)
        var extra = len(mine) - this.opts.Retain
        for i in 0..extra {
            if std.fs.Join(dir, mine[i]) != this.current {
                _ = std.fs.Remove(std.fs.Join(dir, mine[i]))
            }
        }
    }

    func periodOf(ms int) string {
        var r = this.opts.RollingInterval
        if r == RollingInterval.Day {
            return std.time.Format(ms, "%Y%m%d")
        }
        if r == RollingInterval.Hour {
            return std.time.Format(ms, "%Y%m%d%H")
        }
        if r == RollingInterval.Month {
            return std.time.Format(ms, "%Y%m")
        }
        if r == RollingInterval.Year {
            return std.time.Format(ms, "%Y")
        }
        return ""
    }

    // Emit appends e as one line (text or CLEF JSON).
    func Emit(e *Event) {
        var line = ""
        if this.opts.Json {
            line = clef(e)
        } else {
            line = renderOutput(this.segs, e, false)
        }
        std.task.rtLock(&this.guard)
        var period = this.periodOf(e.Timestamp)
        if this.fd < 0 || period != this.period {
            if period != this.period {
                this.seq = 0
            }
            this.open(period)
        } else if this.opts.FileSizeLimit > 0 &&
            this.size + len(line) > this.opts.FileSizeLimit &&
            this.size > 0 {
            this.seq += 1
            this.open(period)
        }
        if this.fd >= 0 {
            _ = rtWrite(this.fd, line)
            this.size += len(line)
        }
        std.task.rtUnlock(&this.guard)
    }

    // Close closes the current file.
    func Close() Task<void> {
        std.task.rtLock(&this.guard)
        if this.fd >= 0 {
            rtClose(this.fd)
            this.fd = -1
        }
        std.task.rtUnlock(&this.guard)
        return std.task.Delay(0)
    }

    // The file being written ("" before the first event).
    func Path() string {
        return this.current
    }
}

// NewFileSink returns a sink appending to path with opts.
func NewFileSink(path string, opts FileOptions) *FileSink {
    var t = opts.Template
    if t == "" {
        t = "{Timestamp:yyyy-MM-dd HH:mm:ss.fff zzz} [{Level:u3}] {Message:lj}{NewLine}{Exception}"
    }
    return new FileSink{base: path, opts: opts, segs: parseOutput(t), fd: -1}
}

// JsonOptions configures NewJsonSink.
struct JsonOptions {
    // File to append to ("" = the console).
    Path string
    // On the console: stdout instead of stderr.
    Stdout bool
}

// JsonSink writes compact JSON (CLEF) lines to a file or the console.
struct JsonSink {
    file   *FileSink
    stdout bool

    // Emit writes e as one CLEF JSON line.
    func Emit(e *Event) {
        if this.file != nil {
            this.file.Emit(e)
            return
        }
        var s = clef(e)
        if this.stdout {
            std.io.print(s)
        } else {
            std.io.Eprint(s)
        }
    }

    // Close closes the file, if any.
    func Close() Task<void> {
        if this.file != nil {
            return this.file.Close()
        }
        return std.task.Delay(0)
    }
}

// NewJsonSink returns a JSON sink with opts.
func NewJsonSink(opts JsonOptions) *JsonSink {
    var s = new JsonSink{stdout: opts.Stdout}
    if opts.Path != "" {
        s.file = NewFileSink(opts.Path, FileOptions{Json: true})
    }
    return s
}

// AsyncSink hands events to a background task through a channel (Emit
// never blocks the caller while the queue has room); Close drains it.
struct AsyncSink {
    ch     std.task.Channel<*Event>
    done   Task<void>
    closed bool

    // Emit queues e for the background task (dropped after Close).
    func Emit(e *Event) {
        if !this.closed {
            _ = this.ch.Send(e)
        }
    }

    // Close stops accepting events, drains the queue and closes the inner sink.
    func Close() Task<void> {
        if !this.closed {
            this.closed = true
            this.ch.Close()
        }
        return this.done
    }
}

async func pump(ch std.task.Channel<*Event>, inner Sink) {
    for {
        var e = try await ch.Recv() else break
        inner.Emit(e)
    }
    await inner.Close()
}

// NewAsyncSink wraps inner with a queue of capacity events (default 1024 when <= 0).
func NewAsyncSink(inner Sink, capacity int) *AsyncSink {
    var n = capacity
    if n <= 0 {
        n = 1024
    }
    var ch = std.task.Channel<*Event>(n)
    return new AsyncSink{ch: ch, done: pump(ch, inner)}
}
