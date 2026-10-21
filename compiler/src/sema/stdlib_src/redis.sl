// std.data.redis: a Redis client written in Slang over std.net and
// std.net.tls (docs/STDLIB.md level 3b). Four files share the namespace:
//
//   redis.sl          RESP2/RESP3 codec (Reply), Options and URLs,
//                     connections (handshake: HELLO / AUTH / SELECT) and
//                     the connection pool (health checks, lifetime, waits
//                     with a timeout and cancellation)
//   redis_client.sl   Client and the typed command set (Cmds), pipelines,
//                     MULTI/EXEC transactions with WATCH, Lua scripts,
//                     pub/sub subscriptions
//   redis_route.sl    the router behind a Client: one server, Sentinel
//                     (master discovery, +switch-master, replicas) or
//                     Cluster (slot map, MOVED/ASK, hash tags), retries
//
// Errors: a server error reply is an Error{Code, Text} ("WRONGTYPE ...");
// a missing value is the sentinel Nil (errors.Is(err, redis.Nil)); network
// errors are those of std.net. The sentinels Nil, ErrClosed, ErrTxFailed,
// ErrPoolTimeout and ErrCrossSlot are compiler constants (sema/stdlib.rs,
// runtime/src/std_infra.c).
namespace std.data.redis

// ---- replies ------------------------------------------------------------------

// The type of a reply: the RESP2 types (Status, Error, Int, Bulk, Array,
// Nil) and the RESP3 additions (Map, Set, Double, Bool, BigNumber,
// Verbatim, Push).
enum Kind {
    Nil
    Status
    Error
    Int
    Bulk
    Array
    Map
    Set
    Double
    Bool
    BigNumber
    Verbatim
    Push
}

// The name of a reply kind ("bulk", "map"...).
func KindName(k Kind) string {
    switch k {
        case Kind.Nil:
            return "nil"
        case Kind.Status:
            return "status"
        case Kind.Error:
            return "error"
        case Kind.Int:
            return "int"
        case Kind.Bulk:
            return "bulk"
        case Kind.Array:
            return "array"
        case Kind.Map:
            return "map"
        case Kind.Set:
            return "set"
        case Kind.Double:
            return "double"
        case Kind.Bool:
            return "bool"
        case Kind.BigNumber:
            return "bignumber"
        case Kind.Verbatim:
            return "verbatim"
    }
    return "push"
}


// One reply. Str holds the text of Status, Error, Bulk, Verbatim (without
// its "txt:" format prefix) and BigNumber replies; Int an Int (and a Bool
// as 0/1); Float a Double; Elems the elements of an Array, Set or Push,
// and of a Map as key, value, key, value...
struct Reply {
    Kind Kind
    Str string
    Int int
    Float float
    Elems []Reply

    func IsNil() bool {
        return this.Kind == Kind.Nil
    }

    func IsError() bool {
        return this.Kind == Kind.Error
    }

    // The error of an Error reply (nil otherwise).
    func Err() error {
        if this.Kind == Kind.Error {
            return errorOf(this.Str)
        }
        return nil
    }

    // The reply as text: Status, Bulk, Verbatim, BigNumber, Int and Double
    // (formatted); Nil fails with redis.Nil, an Error reply with its error.
    func Text() string! {
        switch this.Kind {
            case Kind.Status, Kind.Bulk, Kind.Verbatim, Kind.BigNumber:
                return this.Str
            case Kind.Int:
                return std.strconv.Itoa(this.Int)
            case Kind.Double:
                return formatFloat(this.Float)
            case Kind.Bool:
                if this.Int != 0 {
                    return "1"
                }
                return "0"
            case Kind.Nil:
                return Nil
            case Kind.Error:
                return errorOf(this.Str)
        }
        return error("redis: expected a string reply, got " + KindName(this.Kind))
    }

    // The reply as an integer (an Int, a Bool, or a numeric string).
    func Integer() int! {
        switch this.Kind {
            case Kind.Int, Kind.Bool:
                return this.Int
            case Kind.Status, Kind.Bulk, Kind.BigNumber:
                return std.strconv.Atoi(this.Str)?("redis: integer reply")
            case Kind.Double:
                return int(this.Float)
            case Kind.Nil:
                return Nil
            case Kind.Error:
                return errorOf(this.Str)
        }
        return error("redis: expected an integer reply, got " + KindName(this.Kind))
    }

    // The reply as a float (a Double, an Int or a numeric string).
    func Number() float! {
        switch this.Kind {
            case Kind.Double:
                return this.Float
            case Kind.Int:
                return float(this.Int)
            case Kind.Status, Kind.Bulk:
                return parseFloat(this.Str)?
            case Kind.Nil:
                return Nil
            case Kind.Error:
                return errorOf(this.Str)
        }
        return error("redis: expected a number reply, got " + KindName(this.Kind))
    }

    // The reply as a boolean: Bool, Int (non-zero), Status "OK".
    func Boolean() bool! {
        switch this.Kind {
            case Kind.Bool, Kind.Int:
                return this.Int != 0
            case Kind.Status:
                return this.Str == "OK"
            case Kind.Bulk:
                return this.Str == "1" || this.Str == "OK"
            case Kind.Nil:
                return false
            case Kind.Error:
                return errorOf(this.Str)
        }
        return error("redis: expected a boolean reply, got " + KindName(this.Kind))
    }

    // The elements of an Array, Set, Push or Map (flattened); Nil gives none.
    func Values() []Reply! {
        switch this.Kind {
            case Kind.Array, Kind.Set, Kind.Push, Kind.Map:
                return this.Elems
            case Kind.Nil:
                return []
            case Kind.Error:
                return errorOf(this.Str)
        }
        return error("redis: expected an array reply, got " + KindName(this.Kind))
    }

    // The elements as strings (nil elements become "").
    func Strings() []string! {
        var out []string = []
        for e in this.Values()? {
            if e.Kind == Kind.Nil {
                out = append(out, "")
            } else {
                out = append(out, e.Text()?)
            }
        }
        return out
    }

    // A Map reply, or an Array of key, value pairs (RESP2), as a map.
    func StringMap() map[string]string! {
        var out = map[string]string{}
        var es = this.Values()?
        var i = 0
        for i + 1 < len(es) {
            var v = ""
            if es[i + 1].Kind != Kind.Nil {
                v = es[i + 1].Text()?
            }
            out[es[i].Text()?] = v
            i += 2
        }
        return out
    }

    // A Map reply (or Array of pairs) as a map of replies.
    func ReplyMap() map[string]Reply! {
        var out = map[string]Reply{}
        var es = this.Values()?
        var i = 0
        for i + 1 < len(es) {
            out[es[i].Text()?] = es[i + 1]
            i += 2
        }
        return out
    }

    // The reply as redis-cli would show it (for logs and tests).
    func String() string {
        switch this.Kind {
            case Kind.Nil:
                return "(nil)"
            case Kind.Status:
                return this.Str
            case Kind.Error:
                return "(error) " + this.Str
            case Kind.Int:
                return "(integer) " + std.strconv.Itoa(this.Int)
            case Kind.Double:
                return "(double) " + formatFloat(this.Float)
            case Kind.Bool:
                if this.Int != 0 {
                    return "(true)"
                }
                return "(false)"
            case Kind.Bulk, Kind.Verbatim, Kind.BigNumber:
                return std.strconv.Quote(this.Str)
        }
        var parts []string = []
        if this.Kind == Kind.Map {
            var i = 0
            for i + 1 < len(this.Elems) {
                parts = append(parts, this.Elems[i].String() + " => " + this.Elems[i + 1].String())
                i += 2
            }
            return "{" + std.strings.Join(parts, ", ") + "}"
        }
        for e in this.Elems {
            parts = append(parts, e.String())
        }
        if this.Kind == Kind.Set {
            return "~[" + std.strings.Join(parts, ", ") + "]"
        }
        if this.Kind == Kind.Push {
            return ">[" + std.strings.Join(parts, ", ") + "]"
        }
        return "[" + std.strings.Join(parts, ", ") + "]"
    }
}

// A server error reply: Code is its first word ("ERR", "WRONGTYPE",
// "MOVED", "NOSCRIPT"...), Text the whole message.
struct Error {
    Code string
    Text string

    func Message() string {
        return this.Text
    }
}

func errorOf(text string) error {
    var code = text
    var sp = std.strings.Index(text, " ")
    if sp >= 0 {
        code = text[..sp]
    }
    return Error{Code: code, Text: text}
}

// The code of a server error in err's chain ("" if err is no Error).
func ErrorCode(err error) string {
    var e Error
    if std.errors.As(err, &e) {
        return e.Code
    }
    return ""
}

func formatFloat(f float) string {
    if std.math.IsInf(f, 1) {
        return "inf"
    }
    if std.math.IsInf(f, -1) {
        return "-inf"
    }
    return std.strconv.FormatFloat(f, 'g', -1)
}

func parseFloat(s string) float! {
    var l = std.strings.ToLower(s)
    if l == "inf" || l == "+inf" {
        return std.math.Inf(1)
    }
    if l == "-inf" {
        return std.math.Inf(-1)
    }
    return std.strconv.ParseFloat(s)?("redis: float reply")
}

// ---- RESP codec -----------------------------------------------------------------

// The buffer being parsed; need and err describe an incomplete or bad reply.
struct parser {
    b []byte
    // bytes the incomplete reply needs at least (from its start; 0 unknown)
    need int
    err string
}

// The integer in b[from..to].
func atoiBytes(b []byte, from int, to int) (int, bool) {
    var i = from
    var neg = false
    if i < to && (b[i] == 45 || b[i] == 43) {
        neg = b[i] == 45
        i += 1
    }
    if i >= to {
        return (0, false)
    }
    var n = 0
    for i < to {
        var c = int(b[i])
        if c < 48 || c > 57 {
            return (0, false)
        }
        n = n * 10 + (c - 48)
        i += 1
    }
    if neg {
        n = -n
    }
    return (n, true)
}

func textOf(b []byte, from int, to int) string {
    return std.strings.FromBytes(b[from..to])
}

// Parses one reply at ps.b[p..]: (reply, offset after it). The offset is
// -1 if the buffer ends first (ps.need: total bytes needed from p, when
// known) and -2 on a protocol error (ps.err). RESP3 attributes are
// skipped.
func parse(ps *parser, p int) (Reply, int) {
    var none Reply
    var b = ps.b
    if p >= len(b) {
        return (none, -1)
    }
    var lf = std.net.rtIndexByte(b, p, 10)
    if lf < 0 {
        return (none, -1)
    }
    var end = lf
    if end > p && b[end - 1] == 13 {
        end -= 1
    }
    var t = int(b[p])
    var next = lf + 1
    switch t {
        case 43:
            return (Reply{Kind: Kind.Status, Str: textOf(b, p + 1, end)}, next)
        case 45:
            return (Reply{Kind: Kind.Error, Str: textOf(b, p + 1, end)}, next)
        case 58:
            var (n, ok) = atoiBytes(b, p + 1, end)
            if !ok {
                ps.err = "bad integer " + std.strconv.Quote(textOf(b, p + 1, end))
                return (none, -2)
            }
            return (Reply{Kind: Kind.Int, Int: n}, next)
        case 95:
            return (Reply{Kind: Kind.Nil}, next)
        case 44:
            var s = textOf(b, p + 1, end)
            if std.strings.ToLower(s) == "nan" {
                return (Reply{Kind: Kind.Double, Float: std.math.NaN()}, next)
            }
            var f = try parseFloat(s) catch _ {
                ps.err = "bad double " + std.strconv.Quote(s)
                return (none, -2)
            }
            return (Reply{Kind: Kind.Double, Float: f}, next)
        case 35:
            var v = 0
            if end > p + 1 && b[p + 1] == 116 {
                v = 1
            }
            return (Reply{Kind: Kind.Bool, Int: v}, next)
        case 40:
            return (Reply{Kind: Kind.BigNumber, Str: textOf(b, p + 1, end)}, next)
        case 36, 33, 61:
            var (n, ok) = atoiBytes(b, p + 1, end)
            if !ok {
                ps.err = "bad bulk length"
                return (none, -2)
            }
            if n < 0 {
                return (Reply{Kind: Kind.Nil}, next)
            }
            if next + n + 2 > len(b) {
                ps.need = next + n + 2 - p
                return (none, -1)
            }
            var s = textOf(b, next, next + n)
            var k = Kind.Bulk
            if t == 33 {
                k = Kind.Error
            } else if t == 61 {
                k = Kind.Verbatim
                if len(s) >= 4 && s[3] == 58 {
                    s = s[4..]
                }
            }
            return (Reply{Kind: k, Str: s}, next + n + 2)
        case 42, 37, 126, 62, 124:
            var (n, ok) = atoiBytes(b, p + 1, end)
            if !ok {
                ps.err = "bad aggregate length"
                return (none, -2)
            }
            if n < 0 {
                return (Reply{Kind: Kind.Nil}, next)
            }
            var count = n
            if t == 37 || t == 124 {
                count = 2 * n
            }
            var elems []Reply = []
            var q = next
            for _ in 0..count {
                var (e, nq) = parse(ps, q)
                if nq < 0 {
                    if nq == -1 && ps.need > 0 {
                        ps.need += q - p
                    }
                    return (none, nq)
                }
                elems = append(elems, e)
                q = nq
            }
            if t == 124 {
                // an attribute: the reply it annotates follows
                var (r, nq) = parse(ps, q)
                if nq == -1 && ps.need > 0 {
                    ps.need += q - p
                }
                return (r, nq)
            }
            var k = Kind.Array
            if t == 37 {
                k = Kind.Map
            } else if t == 126 {
                k = Kind.Set
            } else if t == 62 {
                k = Kind.Push
            }
            return (Reply{Kind: k, Elems: elems}, q)
    }
    ps.err = "unexpected reply type byte " + std.strconv.Itoa(t)
    return (none, -2)
}

// Parses a whole buffer of replies (for tests and tools): every reply in
// data, or an error if the data is truncated or malformed.
func ParseReplies(data []byte) []Reply! {
    var ps = parser{b: data}
    var out []Reply = []
    var p = 0
    for p < len(data) {
        var (r, next) = parse(&ps, p)
        if next == -2 {
            return error("redis: protocol error: " + ps.err)
        }
        if next < 0 {
            return error("redis: truncated reply")
        }
        out = append(out, r)
        p = next
    }
    return out
}

// The text of a command argument: strings and []byte as they are,
// numbers in decimal (floats shortest), bools as 1/0, a time.Duration in
// milliseconds, other values with fmt "{0}".
func argBytes(a any) []byte {
    switch a {
        case string s:
            return std.strings.Bytes(s)
        case []byte b:
            return b
        case int n:
            return std.strings.Bytes(std.strconv.Itoa(n))
        case float f:
            return std.strings.Bytes(formatFloat(f))
        case bool v:
            if v {
                return std.strings.Bytes("1")
            }
            return std.strings.Bytes("0")
        case std.time.Duration d:
            return std.strings.Bytes(std.strconv.Itoa(d.Milliseconds()))
    }
    if a == nil {
        return []
    }
    return std.strings.Bytes(std.fmt.Format("{0}", a))
}

// Appends one command in RESP form (an array of bulk strings) to b.
func appendCommand(b []byte, args []any) []byte {
    var out = std.data.rtAppendStr(b, "*" + std.strconv.Itoa(len(args)) + "\r\n")
    for a in args {
        var x = argBytes(a)
        out = std.data.rtAppendStr(out, "$" + std.strconv.Itoa(len(x)) + "\r\n")
        out = std.data.rtAppend(out, x)
        out = std.data.rtAppendStr(out, "\r\n")
    }
    return out
}

// A command in RESP form (for tests and tools).
func EncodeCommand(args []any) []byte {
    return appendCommand([], args)
}

// ---- options and URLs ------------------------------------------------------------

// Options of a Client (and of each node of a Sentinel or Cluster client).
// Durations are milliseconds; zero values take the defaults in brackets.
struct Options {
    // host:port [localhost:6379], or a Unix socket path when Network is "unix"
    Addr string
    Network string
    Username string
    Password string
    DB int
    // RESP version: 3 (HELLO 3), 2, or 0: try 3, fall back to 2
    Protocol int
    ClientName string
    // TLS for rediss:// (nil: plain TCP)
    TLS *std.net.tls.Config
    DialTimeout int
    // limit for every network wait of a command (0: none; the task
    // context's deadline and cancellation apply as well)
    ReadTimeout int
    // connections per server [10]
    PoolSize int
    // waiting for a free connection [ReadTimeout + 1000, at least 4000]
    PoolTimeout int
    // idle connections older than this are checked with PING before use
    // [30000; -1 never]
    IdleCheck int
    // connections are closed after this age (0: no limit)
    MaxConnAge int
    // retries of a command after a network error or a LOADING / TRYAGAIN /
    // CLUSTERDOWN / MASTERDOWN / READONLY reply [3; -1 none]
    MaxRetries int
    // exponential backoff between retries [8 .. 512]
    MinRetryBackoff int
    MaxRetryBackoff int
}

func withDefaults(o Options) Options {
    var r = o
    if r.Addr == "" {
        r.Addr = "localhost:6379"
    }
    if r.Network == "" {
        r.Network = "tcp"
    }
    if r.DialTimeout == 0 {
        r.DialTimeout = 5000
    }
    if r.PoolSize <= 0 {
        r.PoolSize = 10
    }
    if r.PoolTimeout == 0 {
        r.PoolTimeout = r.ReadTimeout + 1000
        if r.PoolTimeout < 4000 {
            r.PoolTimeout = 4000
        }
    }
    if r.IdleCheck == 0 {
        r.IdleCheck = 30000
    }
    if r.MaxRetries == 0 {
        r.MaxRetries = 3
    }
    if r.MinRetryBackoff <= 0 {
        r.MinRetryBackoff = 8
    }
    if r.MaxRetryBackoff <= 0 {
        r.MaxRetryBackoff = 512
    }
    return r
}

// "500ms", "5s", "1m" or a number of seconds, in milliseconds.
func parseMs(v string) int! {
    var n = try std.strconv.ParseFloat(v) catch _ {
        var d = std.time.ParseDuration(v)?("redis: bad duration " + std.strconv.Quote(v))
        return d.Milliseconds()
    }
    return int(n * 1000)
}

// Parses redis://[[user]:password@]host[:port][/db][?options],
// rediss://... (TLS) or unix:///path/to/socket[?db=N&...]. Options:
// db, protocol, client_name, dial_timeout, read_timeout, pool_size,
// pool_timeout, idle_check, max_conn_age, max_retries (durations as
// "500ms", "5s" or seconds), and for rediss skip_verify=true.
func ParseURL(raw string) Options! {
    var u = std.net.url.Parse(raw)?("redis: invalid URL")
    var o = Options{}
    var scheme = std.strings.ToLower(u.Scheme)
    if scheme == "unix" {
        o.Network = "unix"
        o.Addr = u.Path
    } else if scheme == "redis" || scheme == "rediss" {
        var host = u.Host
        if host == "" {
            host = "localhost"
        }
        var port = u.Port
        if port == "" {
            port = "6379"
        }
        o.Addr = std.net.JoinHostPort(host, std.strconv.Atoi(port)?("redis: invalid port"))
        var path = std.strings.Trim(u.Path)
        if std.strings.HasPrefix(path, "/") {
            path = path[1..]
        }
        if path != "" {
            o.DB = std.strconv.Atoi(path)?("redis: invalid database number " + std.strconv.Quote(path))
        }
        if scheme == "rediss" {
            o.TLS = &std.net.tls.Config{ServerName: host}
        }
    } else {
        return error("redis: invalid URL scheme " + std.strconv.Quote(u.Scheme) + " (want redis, rediss or unix)")
    }
    if u.User != nil {
        o.Username = u.User.Username
        if u.User.HasPassword {
            o.Password = u.User.Password
        } else if o.Username != "" {
            // redis://secret@host: the password alone (Redis < 6 style)
            o.Password = o.Username
            o.Username = ""
        }
    }
    var q = std.net.url.ParseQuery(u.RawQuery)?("redis: invalid URL query")
    var db = q.Get("db")
    if db != "" {
        o.DB = std.strconv.Atoi(db)?("redis: invalid db")
    }
    var proto = q.Get("protocol")
    if proto != "" {
        o.Protocol = std.strconv.Atoi(proto)?("redis: invalid protocol")
        if o.Protocol != 2 && o.Protocol != 3 {
            return error("redis: protocol must be 2 or 3")
        }
    }
    o.ClientName = q.Get("client_name")
    if q.Get("dial_timeout") != "" {
        o.DialTimeout = parseMs(q.Get("dial_timeout"))?
    }
    if q.Get("read_timeout") != "" {
        o.ReadTimeout = parseMs(q.Get("read_timeout"))?
    }
    if q.Get("pool_timeout") != "" {
        o.PoolTimeout = parseMs(q.Get("pool_timeout"))?
    }
    if q.Get("idle_check") != "" {
        o.IdleCheck = parseMs(q.Get("idle_check"))?
    }
    if q.Get("max_conn_age") != "" {
        o.MaxConnAge = parseMs(q.Get("max_conn_age"))?
    }
    if q.Get("pool_size") != "" {
        o.PoolSize = std.strconv.Atoi(q.Get("pool_size"))?("redis: invalid pool_size")
    }
    if q.Get("max_retries") != "" {
        o.MaxRetries = std.strconv.Atoi(q.Get("max_retries"))?("redis: invalid max_retries")
    }
    if o.TLS != nil && (q.Get("skip_verify") == "true" || q.Get("insecure") == "true") {
        o.TLS.InsecureSkipVerify = true
    }
    return o
}

// ---- connections -----------------------------------------------------------------

// A std.net.Conn or a std.net.tls.Conn.
interface stream {
    func Read(n int) Task<[]byte!>
    func Write(data []byte) Task<int!>
    func Close()
    func SetTimeout(ms int)
}

func nowMs() int {
    return std.diag.rtMonoNanos() / 1000000
}

// One server connection. Not safe for concurrent use: the pool hands it
// to one caller at a time.
struct conn {
    s stream
    addr string
    buf []byte
    pos int
    bad bool
    closed bool
    proto int
    created int
    used int
    gen int

    func close() {
        if !this.closed {
            this.closed = true
            this.s.Close()
        }
    }

    async func send(b []byte) void! {
        try await this.s.Write(b) catch err {
            this.bad = true
            return err
        }
    }

    // Reads more bytes into the buffer (fails at the end of the stream).
    async func more() void! {
        var chunk = try await this.s.Read(65536) catch err {
            this.bad = true
            return err
        }
        if len(chunk) == 0 {
            this.bad = true
            return std.errors.Wrap(std.net.ErrEOF, "redis: connection closed by the server")
        }
        if this.pos >= len(this.buf) {
            this.buf = chunk
        } else if this.pos > 0 {
            this.buf = std.net.rtJoin(this.buf[this.pos..], chunk, len(chunk))
        } else {
            this.buf = std.data.rtAppend(this.buf, chunk)
        }
        this.pos = 0
    }

    // The next reply.
    async func read() Reply! {
        for {
            var ps = parser{b: this.buf}
            var (r, next) = parse(&ps, this.pos)
            if next >= 0 {
                this.pos = next
                if this.pos >= len(this.buf) {
                    this.buf = []
                    this.pos = 0
                }
                return r
            }
            if next == -2 {
                this.bad = true
                return error("redis: protocol error: " + ps.err)
            }
            // read until the reply can be complete
            await this.more()?
            for ps.need > 0 && len(this.buf) - this.pos < ps.need {
                await this.more()?
            }
        }
    }

    // Sends the commands in one write and reads one reply per command
    // (a server error is an Error reply, not a failure).
    async func pipeline(cmds [][]any) []Reply! {
        var b []byte = []
        for c in cmds {
            b = appendCommand(b, c)
        }
        await this.send(b)?
        var out []Reply = []
        for _ in cmds {
            out = append(out, await this.read()?)
        }
        this.used = nowMs()
        return out
    }

    async func do(args []any) Reply! {
        await this.send(appendCommand([], args))?
        var r = await this.read()?
        this.used = nowMs()
        return r
    }

    // HELLO (RESP3) or AUTH, then SELECT and CLIENT SETNAME.
    async func handshake(o Options) void! {
        this.proto = 2
        var pending [][]any = []
        if o.Protocol != 2 {
            var hello []any = ["HELLO", "3"]
            if o.Password != "" {
                var user = o.Username
                if user == "" {
                    user = "default"
                }
                hello = append(hello, "AUTH")
                hello = append(hello, user)
                hello = append(hello, o.Password)
            }
            if o.ClientName != "" {
                hello = append(hello, "SETNAME")
                hello = append(hello, o.ClientName)
            }
            var r = await this.do(hello)?
            if r.Kind == Kind.Error {
                var code = ErrorCode(r.Err())
                if o.Protocol == 3 || code == "WRONGPASS" || code == "NOAUTH" || code == "NOPERM" {
                    return r.Err()
                }
            } else {
                this.proto = 3
            }
        }
        if this.proto == 2 {
            if o.Password != "" {
                if o.Username != "" {
                    pending = append(pending, ["AUTH", o.Username, o.Password])
                } else {
                    pending = append(pending, ["AUTH", o.Password])
                }
            }
            if o.ClientName != "" {
                pending = append(pending, ["CLIENT", "SETNAME", o.ClientName])
            }
        }
        if o.DB != 0 {
            pending = append(pending, ["SELECT", o.DB])
        }
        if len(pending) > 0 {
            for r in await this.pipeline(pending)? {
                if r.Kind == Kind.Error {
                    return r.Err()
                }
            }
        }
    }
}

// Opens and initializes a connection to addr.
async func dial(o Options, addr string) *conn! {
    var raw std.net.Conn
    if o.Network == "unix" {
        raw = await std.net.DialUnix(addr)?
    } else if o.DialTimeout > 0 {
        raw = await std.net.DialTimeout(addr, o.DialTimeout)?
    } else {
        raw = await std.net.Dial(addr)?
    }
    var s stream = raw
    if o.TLS != nil {
        var cfg = *o.TLS
        if cfg.ServerName == "" {
            var hp = try std.net.SplitHostPort(addr) catch _ {
                raw.Close()
                return error("redis: bad address " + addr)
            }
            cfg.ServerName = hp.Host
        }
        var tc = try await std.net.tls.Client(raw, cfg) catch err {
            raw.Close()
            return err
        }
        try await tc.Handshake() catch err {
            tc.Close()
            return err
        }
        s = tc
    }
    if o.ReadTimeout > 0 {
        s.SetTimeout(o.ReadTimeout)
    }
    var t = nowMs()
    var c = new conn{s: s, addr: addr, created: t, used: t}
    try await c.handshake(o) catch err {
        c.close()
        return err
    }
    return c
}

// ---- the connection pool ----------------------------------------------------------

// Pool statistics of a Client (summed over its servers).
struct PoolStats {
    // connections handed out from the idle list / newly dialed
    Hits int
    Misses int
    // waits for a free connection that timed out
    Timeouts int
    TotalConns int
    IdleConns int
    // idle connections closed by the health check or MaxConnAge
    StaleConns int
}

struct poolWaiter {
    id int
    ch std.task.Channel<int>
}

// The connections to one server.
struct pool {
    guard int
    addr string
    opts Options
    idle []*conn
    numOpen int
    waiters []poolWaiter
    nextWaiter int
    closed bool
    gen int
    stats PoolStats

    func lock() {
        std.task.rtLock(&this.guard)
    }

    func unlock() {
        std.task.rtUnlock(&this.guard)
    }

    // Counts a hit (0), a timeout (1) or a stale connection (2).
    func count(what int) {
        this.lock()
        if what == 0 {
            this.stats.Hits += 1
        } else if what == 1 {
            this.stats.Timeouts += 1
        } else {
            this.stats.StaleConns += 1
        }
        this.unlock()
    }

    // Under the lock.
    func wakeOne() {
        if len(this.waiters) > 0 {
            var w = this.waiters[0]
            this.waiters = this.waiters[1..]
            _ = w.ch.Send(1)
        }
    }

    // A connection: an idle one (checked with PING when it has been idle
    // longer than IdleCheck), a new one while fewer than PoolSize are
    // open, else the next one given back (ErrPoolTimeout after
    // PoolTimeout).
    async func get() *conn! {
        var deadline = nowMs() + this.opts.PoolTimeout
        for {
            std.task.CheckCancelled()?
            var now = nowMs()
            this.lock()
            if this.closed {
                this.unlock()
                return ErrClosed
            }
            if len(this.idle) > 0 {
                var n = len(this.idle)
                var c = this.idle[n - 1]
                this.idle = this.idle[..n - 1]
                this.unlock()
                if this.opts.MaxConnAge > 0 && now - c.created >= this.opts.MaxConnAge {
                    this.count(2)
                    this.remove(c)
                    continue
                }
                if this.opts.IdleCheck > 0 && now - c.used >= this.opts.IdleCheck {
                    var ok = true
                    try {
                        var r = await c.do(["PING"])?
                        ok = r.Kind != Kind.Error
                    } catch _ {
                        ok = false
                    }
                    if !ok {
                        this.count(2)
                        this.remove(c)
                        continue
                    }
                }
                this.count(0)
                return c
            }
            if this.numOpen < this.opts.PoolSize {
                this.numOpen += 1
                var gen = this.gen
                var addr = this.addr
                this.stats.Misses += 1
                this.unlock()
                var c = try await dial(this.opts, addr) catch err {
                    this.lock()
                    this.numOpen -= 1
                    this.wakeOne()
                    this.unlock()
                    return err
                }
                c.gen = gen
                return c
            }
            this.nextWaiter += 1
            var w = poolWaiter{id: this.nextWaiter, ch: std.task.Channel<int>(1)}
            this.waiters = append(this.waiters, w)
            this.unlock()
            var left = deadline - nowMs()
            var woken = false
            if left > 0 {
                woken = await std.task.Timeout(w.ch.Recv(), left)
            }
            if !woken {
                this.lock()
                var found = false
                var rest []poolWaiter = []
                for x in this.waiters {
                    if x.id == w.id {
                        found = true
                    } else {
                        rest = append(rest, x)
                    }
                }
                this.waiters = rest
                if !found {
                    // woken right at the deadline: pass the wake-up on
                    this.wakeOne()
                }
                this.stats.Timeouts += 1
                this.unlock()
                w.ch.Close()
                std.task.CheckCancelled()?
                return ErrPoolTimeout
            }
        }
    }

    // Gives c back (closes it if it is broken, from an old generation, or
    // the pool is closed).
    func put(c *conn) {
        this.lock()
        if c.bad || c.closed || this.closed || c.gen != this.gen {
            this.numOpen -= 1
            this.wakeOne()
            this.unlock()
            c.close()
            return
        }
        this.idle = append(this.idle, c)
        this.wakeOne()
        this.unlock()
    }

    // Closes a connection taken from the pool for good.
    func remove(c *conn) {
        c.close()
        this.lock()
        this.numOpen -= 1
        this.wakeOne()
        this.unlock()
    }

    // Points the pool at another server (Sentinel failover): idle
    // connections are closed, connections in use are closed when they
    // come back.
    func setAddr(addr string) {
        this.lock()
        if this.addr == addr {
            this.unlock()
            return
        }
        this.addr = addr
        this.gen += 1
        var old = this.idle
        this.idle = []
        this.numOpen -= len(old)
        for _ in old {
            this.wakeOne()
        }
        this.unlock()
        for c in old {
            c.close()
        }
    }

    func currentAddr() string {
        this.lock()
        var a = this.addr
        this.unlock()
        return a
    }

    func close() {
        this.lock()
        this.closed = true
        var old = this.idle
        this.idle = []
        this.numOpen -= len(old)
        var ws = this.waiters
        this.waiters = []
        this.unlock()
        for c in old {
            c.close()
        }
        for w in ws {
            _ = w.ch.Send(1)
        }
    }

    func snapshot() PoolStats {
        this.lock()
        var s = this.stats
        s.TotalConns = this.numOpen
        s.IdleConns = len(this.idle)
        this.unlock()
        return s
    }
}

func newPool(o Options, addr string) *pool {
    return new pool{addr: addr, opts: o}
}
