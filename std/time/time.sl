namespace std.time

// std.time types (Go's time package): Duration, Time, Location, Instant,
// Ticker and Timer. The older int-millisecond functions (time.Now(),
// time.Format(ms, strftime), time.Year(ms), ...) are runtime externs and
// stay as they are; because of them a few Go names differ here:
//
//   Go                         Slang
//   time.Now()                 time.Time.Now() / time.Current()
//   time.Since(t)              time.Elapsed(t)
//   time.Sleep(d)              time.SleepFor(d) (async code: task.Delay)
//   time.Parse(layout, s)      time.ParseTime(layout, s)
//   time.Second, time.Hour...  time.Seconds(n), time.Hours(n), ... (Duration
//                              constructors; constants cannot be values of
//                              stdlib struct types)
//   t.Month() Month            t.Month() int (1..12), t.MonthName()
//   t.Weekday() Weekday        t.Weekday() int (0 = Sunday), t.WeekdayName()
//
// Time counts seconds since January 1, year 1 UTC (like Go internally), so
// the zero value is Go's zero time and IsZero is true for it.

// ---- Duration ---------------------------------------------------------------

// An elapsed time in nanoseconds (int64 range, about ±292 years).
struct Duration {
    ns int

    func Nanoseconds() int {
        return this.ns
    }

    // Whole microseconds (truncated toward zero).
    func Microseconds() int {
        return this.ns / 1000
    }

    // Whole milliseconds (truncated toward zero).
    func Milliseconds() int {
        return this.ns / 1000000
    }

    func Seconds() float {
        return float(this.ns / 1000000000) + float(this.ns % 1000000000) / 1000000000.0
    }

    func Minutes() float {
        return float(this.ns / 60000000000) + float(this.ns % 60000000000) / 60000000000.0
    }

    func Hours() float {
        return float(this.ns / 3600000000000) + float(this.ns % 3600000000000) / 3600000000000.0
    }

    func Add(d Duration) Duration {
        return Duration{ns: this.ns + d.ns}
    }

    func Sub(d Duration) Duration {
        return Duration{ns: this.ns - d.ns}
    }

    func Mul(n int) Duration {
        return Duration{ns: this.ns * n}
    }

    func Div(n int) Duration {
        return Duration{ns: this.ns / n}
    }

    func Neg() Duration {
        return Duration{ns: -this.ns}
    }

    func Abs() Duration {
        if this.ns < 0 {
            return Duration{ns: -this.ns}
        }
        return *this
    }

    // Rounded toward zero to a multiple of m (unchanged if m <= 0).
    func Truncate(m Duration) Duration {
        if m.ns <= 0 {
            return *this
        }
        return Duration{ns: this.ns - this.ns % m.ns}
    }

    // Rounded to the nearest multiple of m, halfway away from zero.
    func Round(m Duration) Duration {
        if m.ns <= 0 {
            return *this
        }
        var r = this.ns % m.ns
        if this.ns < 0 {
            r = -r
            if r + r < m.ns {
                return Duration{ns: this.ns + r}
            }
            return Duration{ns: this.ns + r - m.ns}
        }
        if r + r < m.ns {
            return Duration{ns: this.ns - r}
        }
        return Duration{ns: this.ns + m.ns - r}
    }

    func Compare(o Duration) int {
        if this.ns < o.ns {
            return -1
        }
        if this.ns > o.ns {
            return 1
        }
        return 0
    }

    func Equals(o Duration) bool {
        return this.ns == o.ns
    }

    // Go's format: "72h3m0.5s", "1.5s", "300ms", "2µs", "0s".
    func String() string {
        var d = this.ns
        if d == 0 {
            return "0s"
        }
        var neg = d < 0
        var u = uint(d)
        if neg {
            u = uint(0) - u
        }
        var out = ""
        if u < 1000000000 {
            // below one second: ns, µs or ms with a fraction
            if u < 1000 {
                out = std.strconv.FormatUint(u, 10) + "ns"
            } else if u < 1000000 {
                out = fracText(u, 3) + "µs"
            } else {
                out = fracText(u, 6) + "ms"
            }
        } else {
            var secs = u / 1000000000
            var frac = u % 1000000000
            var h = secs / 3600
            var m = (secs / 60) % 60
            var s = secs % 60
            var stext = fracText(s * 1000000000 + frac, 9) + "s"
            if h > 0 {
                out = std.strconv.FormatUint(h, 10) +
                    "h" +
                    std.strconv.FormatUint(m, 10) +
                    "m" +
                    stext
            } else if m > 0 {
                out = std.strconv.FormatUint(m, 10) + "m" + stext
            } else {
                out = stext
            }
        }
        if neg {
            return "-" + out
        }
        return out
    }
}

// v / 10^prec with the fraction's trailing zeros removed ("1.5", "2").
func fracText(v uint, prec int) string {
    var p uint = 1
    for _ in 0..prec {
        p = p * 10
    }
    var whole = std.strconv.FormatUint(v / p, 10)
    var f = v % p
    if f == 0 {
        return whole
    }
    var digits = std.strconv.FormatUint(f, 10)
    for len(digits) < prec {
        digits = "0" + digits
    }
    for std.strings.HasSuffix(digits, "0") {
        digits = digits[..len(digits) - 1]
    }
    return whole + "." + digits
}

func Nanoseconds(n int) Duration {
    return Duration{ns: n}
}

func Microseconds(n int) Duration {
    return Duration{ns: n * 1000}
}

func Milliseconds(n int) Duration {
    return Duration{ns: n * 1000000}
}

func Seconds(n int) Duration {
    return Duration{ns: n * 1000000000}
}

func Minutes(n int) Duration {
    return Duration{ns: n * 60000000000}
}

func Hours(n int) Duration {
    return Duration{ns: n * 3600000000000}
}

// A fractional number of seconds (rounded to the nanosecond).
func SecondsF(s float) Duration {
    return Duration{ns: int(std.math.Round(s * 1000000000.0))}
}

func unitNanos(u string) int {
    switch u {
        case "ns": return 1
        case "us", "µs", "μs": return 1000
        case "ms": return 1000000
        case "s": return 1000000000
        case "m": return 60000000000
        case "h": return 3600000000000
    }
    return 0
}

// Parses Go duration text: an optional sign and a sequence of decimal
// numbers with units ns, us (µs), ms, s, m, h: "300ms", "-1.5h", "2h45m".
func ParseDuration(s string) Duration! {
    var orig = s
    var rest = s
    var neg = false
    if rest != "" && (rest[0] == 45 || rest[0] == 43) {
        neg = rest[0] == 45
        rest = rest[1..]
    }
    if rest == "0" {
        return Duration{}
    }
    if rest == "" {
        return error("time: invalid duration " + std.strconv.Quote(orig))
    }
    var total = 0.0
    var exact = 0
    for rest != "" {
        var i = 0
        for i < len(rest) && ((rest[i] >= 48 && rest[i] <= 57) || rest[i] == 46) {
            i += 1
        }
        if i == 0 {
            return error("time: invalid duration " + std.strconv.Quote(orig))
        }
        var num = rest[..i]
        rest = rest[i..]
        var j = 0
        for j < len(rest) && !((rest[j] >= 48 && rest[j] <= 57) || rest[j] == 46) {
            j += 1
        }
        var unit = rest[..j]
        rest = rest[j..]
        if unit == "" {
            return error("time: missing unit in duration " + std.strconv.Quote(orig))
        }
        var mult = unitNanos(unit)
        if mult == 0 {
            return error(
                "time: unknown unit " +
                    std.strconv.Quote(unit) +
                    " in duration " +
                    std.strconv.Quote(orig),
            )
        }
        if std.strings.Contains(num, ".") {
            var f = try std.strconv.ParseFloat(num) catch err {
                return error("time: invalid duration " + std.strconv.Quote(orig))
            }
            total += f * float(mult)
        } else {
            var n = try std.strconv.ParseInt(num, 10) catch err {
                return error("time: invalid duration " + std.strconv.Quote(orig))
            }
            exact += n * mult
        }
    }
    var ns = exact + int(std.math.Round(total))
    if neg {
        ns = -ns
    }
    return Duration{ns: ns}
}

// ---- calendar ---------------------------------------------------------------

// Seconds from January 1, year 1 to the Unix epoch.
func unixToInternal() int {
    return 62135596800
}

// Days since 1970-01-01 of the civil date y-m-d (proleptic Gregorian).
func daysFromCivil(y0 int, m int, d int) int {
    var y = y0
    if m <= 2 {
        y -= 1
    }
    var era = y / 400
    if y < 0 && y % 400 != 0 {
        era -= 1
    }
    var yoe = y - era * 400
    var mp = (m + 9) % 12
    var doy = (153 * mp + 2) / 5 + d - 1
    var doe = yoe * 365 + yoe / 4 - yoe / 100 + doy
    return era * 146097 + doe - 719468
}

// (year, month, day) of a day count since 1970-01-01.
func civilFromDays(z0 int) (int, int, int) {
    var z = z0 + 719468
    var era = z / 146097
    if z < 0 && z % 146097 != 0 {
        era -= 1
    }
    var doe = z - era * 146097
    var yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365
    var y = yoe + era * 400
    var doy = doe - (365 * yoe + yoe / 4 - yoe / 100)
    var mp = (5 * doy + 2) / 153
    var d = doy - (153 * mp + 2) / 5 + 1
    var m = mp + 3
    if m > 12 {
        m -= 12
    }
    if m <= 2 {
        y += 1
    }
    return (y, m, d)
}

func isLeap(y int) bool {
    return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)
}

func daysIn(m int, y int) int {
    if m == 2 {
        if isLeap(y) {
            return 29
        }
        return 28
    }
    if m == 4 || m == 6 || m == 9 || m == 11 {
        return 30
    }
    return 31
}

func floorDiv(a int, b int) int {
    var q = a / b
    if (a % b != 0) && ((a < 0) != (b < 0)) {
        q -= 1
    }
    return q
}

func floorMod(a int, b int) int {
    return a - floorDiv(a, b) * b
}

// ---- Location ---------------------------------------------------------------

struct zone {
    name   string
    offset int
    isDST  bool
}

// POSIX TZ rule date: kind 0 = Jn (Julian, no Feb 29), 1 = n (0-based
// day of year), 2 = Mm.w.d; time = seconds after local midnight.
struct tzRule {
    kind int
    day  int
    week int
    mon  int
    time int
}

// A time zone: a set of zones and the instants when they change (TZif
// data from the system tzdata), plus the POSIX rule for later years.
struct Location {
    name  string
    zones []zone
    trans []int
    idx   []int
    // POSIX TZ footer: std/dst zones and rules; hasRule false = none.
    hasRule bool
    hasDST  bool
    std     zone
    dst     zone
    start   tzRule
    end     tzRule
    // Local: loaded on first use.
    lazy bool

    // The name: "UTC", "Local" or the IANA name ("Europe/Berlin").
    func String() string {
        if this.name == "" {
            return "UTC"
        }
        return this.name
    }

    func load() {
        if !this.lazy {
            return
        }
        this.lazy = false
        var data = try rtLocalTZif() else ""
        if data == "" {
            return
        }
        var loc = try parseTZif("Local", data) else Location{}
        this.zones = loc.zones
        this.trans = loc.trans
        this.idx = loc.idx
        this.hasRule = loc.hasRule
        this.hasDST = loc.hasDST
        this.std = loc.std
        this.dst = loc.dst
        this.start = loc.start
        this.end = loc.end
    }

    // The zone in effect at sec (seconds since the Unix epoch, UTC).
    func lookup(sec int) zone {
        this.load()
        if len(this.zones) == 0 && !this.hasRule {
            return zone{name: "UTC", offset: 0}
        }
        if len(this.trans) == 0 || sec < this.trans[0] {
            if len(this.trans) == 0 && this.hasRule {
                return ruleZone(this, sec)
            }
            // Go's lookupFirstZone: the first non-DST zone.
            for z in this.zones {
                if !z.isDST {
                    return z
                }
            }
            return this.zones[0]
        }
        var n = len(this.trans)
        if sec >= this.trans[n - 1] && this.hasRule {
            return ruleZone(this, sec)
        }
        var lo = 0
        var hi = n
        for hi - lo > 1 {
            var m = lo + (hi - lo) / 2
            if sec < this.trans[m] {
                hi = m
            } else {
                lo = m
            }
        }
        return this.zones[this.idx[lo]]
    }
}

func ruleDayStart(r tzRule, year int) int {
    // Seconds from the start of the year (local) to the rule's day.
    if r.kind == 0 {
        var d = r.day - 1
        if isLeap(year) && r.day >= 60 {
            d += 1
        }
        return d * 86400
    }
    if r.kind == 1 {
        return r.day * 86400
    }
    var first = daysFromCivil(year, r.mon, 1)
    var jan1 = daysFromCivil(year, 1, 1)
    // weekday of the first of the month (0 = Sunday; 1970-01-01 was Thursday)
    var wd = floorMod(first + 4, 7)
    var dom = 1 + floorMod(r.day - wd, 7) + 7 * (r.week - 1)
    var dim = daysIn(r.mon, year)
    for dom > dim {
        dom -= 7
    }
    return (first - jan1 + dom - 1) * 86400
}

func ruleZone(loc *Location, sec int) zone {
    if !loc.hasDST {
        return loc.std
    }
    var (year, _, _) = civilFromDays(floorDiv(sec + loc.std.offset, 86400))
    var ystart = daysFromCivil(year, 1, 1) * 86400
    var start = ystart + ruleDayStart(loc.start, year) + loc.start.time - loc.std.offset
    var end = ystart + ruleDayStart(loc.end, year) + loc.end.time - loc.dst.offset
    if start < end {
        if sec >= start && sec < end {
            return loc.dst
        }
        return loc.std
    }
    if sec >= end && sec < start {
        return loc.std
    }
    return loc.dst
}

// UTC.
func UTC() *Location {
    return new Location{name: "UTC"}
}

// The system's local time zone ($TZ, else /etc/localtime; UTC if neither
// can be read). Loaded on first use.
func Local() *Location {
    return new Location{name: "Local", lazy: true}
}

// A zone with a fixed offset (seconds east of UTC) and name.
func FixedZone(name string, offset int) *Location {
    var z = zone{name: name, offset: offset}
    return new Location{name: name, zones: [z], trans: [], idx: []}
}

// The IANA zone name ("America/New_York") from the system tzdata
// (/usr/share/zoneinfo or $ZONEINFO). "" and "UTC" give UTC, "Local" the
// local zone.
func LoadLocation(name string) *Location! {
    if name == "" || name == "UTC" {
        return UTC()
    }
    if name == "Local" {
        return Local()
    }
    if std.strings.Contains(name, "..") || std.strings.HasPrefix(name, "/") {
        return error("time: invalid location name " + std.strconv.Quote(name))
    }
    var dir = std.os.Getenv("ZONEINFO")
    if dir == "" {
        dir = "/usr/share/zoneinfo"
    }
    var data = try std.io.ReadFile(dir + "/" + name) catch err {
        return std.errors.Wrap(err, "time: unknown time zone " + name)
    }
    var loc = parseTZif(name, data)?
    var out = new Location{}
    *out = loc
    return out
}

func be32(s string, i int) int {
    var v = (int(s[i]) << 24) | (int(s[i + 1]) << 16) | (int(s[i + 2]) << 8) | int(s[i + 3])
    if v >= 2147483648 {
        v -= 4294967296
    }
    return v
}

func be64(s string, i int) int {
    var v uint = 0
    for k in 0..8 {
        v = (v << 8) | uint(s[i + k])
    }
    return int(v)
}

// Parses TZif data (RFC 8536, versions 1-4; the 64-bit body is used when
// present, and the POSIX footer for times after the last transition).
func parseTZif(name string, data string) Location! {
    var bad = error("time: malformed time zone information for " + name)
    if len(data) < 44 || data[..4] != "TZif" {
        return bad
    }
    var version = data[4]
    var off = 0
    var size = 4
    if version >= 50 {
        // skip the v1 body
        var isutc = be32(data, 20)
        var isstd = be32(data, 24)
        var leap = be32(data, 28)
        var timecnt = be32(data, 32)
        var typecnt = be32(data, 36)
        var charcnt = be32(data, 40)
        off = 44 + timecnt * 5 + typecnt * 6 + charcnt + leap * 8 + isstd + isutc
        size = 8
        if len(data) < off + 44 || data[off..off + 4] != "TZif" {
            return bad
        }
    }
    var isutc = be32(data, off + 20)
    var isstd = be32(data, off + 24)
    var leap = be32(data, off + 28)
    var timecnt = be32(data, off + 32)
    var typecnt = be32(data, off + 36)
    var charcnt = be32(data, off + 40)
    var p = off + 44
    var need = timecnt * size + timecnt + typecnt * 6 + charcnt + leap * (size + 4) + isstd + isutc
    if len(data) < p + need || typecnt == 0 {
        return bad
    }
    var trans []int = []
    for i in 0..timecnt {
        if size == 8 {
            trans = append(trans, be64(data, p + i * 8))
        } else {
            trans = append(trans, be32(data, p + i * 4))
        }
    }
    p += timecnt * size
    var idx []int = []
    for i in 0..timecnt {
        idx = append(idx, int(data[p + i]))
    }
    p += timecnt
    var abbrStart = p + typecnt * 6
    var zones []zone = []
    for i in 0..typecnt {
        var q = p + i * 6
        var offset = be32(data, q)
        var dst = data[q + 4] != 0
        var ai = abbrStart + int(data[q + 5])
        var end = ai
        for end < abbrStart + charcnt && data[end] != 0 {
            end += 1
        }
        zones = append(zones, zone{name: data[ai..end], offset: offset, isDST: dst})
    }
    for i in idx {
        if i >= typecnt {
            return bad
        }
    }
    var loc = Location{name: name, zones: zones, trans: trans, idx: idx}
    p = abbrStart + charcnt + leap * (size + 4) + isstd + isutc
    if size == 8 && p < len(data) && data[p] == 10 {
        var nl = std.strings.Index(data[p + 1..], "\n")
        if nl > 0 {
            parseTZRule(&loc, data[p + 1..p + 1 + nl])
        }
    }
    return loc
}

// ---- POSIX TZ strings ("CET-1CEST,M3.5.0,M10.5.0/3") ---------------------------

struct tzScan {
    s  string
    i  int
    ok bool

    func more() bool {
        return this.i < len(this.s)
    }

    func peek() byte {
        if this.i < len(this.s) {
            return this.s[this.i]
        }
        return 0
    }

    func name() string {
        if this.peek() == 60 {
            var start = this.i + 1
            var j = std.strings.Index(this.s[start..], ">")
            if j < 0 {
                this.ok = false
                return ""
            }
            this.i = start + j + 1
            return this.s[start..start + j]
        }
        var start = this.i
        for this.more() &&
            ((this.peek() >= 65 && this.peek() <= 90) ||
                (this.peek() >= 97 && this.peek() <= 122)) {
            this.i += 1
        }
        if this.i - start < 3 {
            this.ok = false
        }
        return this.s[start..this.i]
    }

    func num() int {
        var start = this.i
        var v = 0
        for this.more() && this.peek() >= 48 && this.peek() <= 57 {
            v = v * 10 + int(this.peek()) - 48
            this.i += 1
        }
        if this.i == start {
            this.ok = false
        }
        return v
    }

    // [+-]hh[:mm[:ss]] in seconds.
    func clock() int {
        var sign = 1
        if this.peek() == 45 {
            sign = -1
            this.i += 1
        } else if this.peek() == 43 {
            this.i += 1
        }
        var v = this.num() * 3600
        if this.peek() == 58 {
            this.i += 1
            v += this.num() * 60
            if this.peek() == 58 {
                this.i += 1
                v += this.num()
            }
        }
        return sign * v
    }

    func rule() tzRule {
        var r = tzRule{time: 7200}
        if this.peek() == 74 {
            this.i += 1
            r.kind = 0
            r.day = this.num()
        } else if this.peek() == 77 {
            this.i += 1
            r.kind = 2
            r.mon = this.num()
            if this.peek() != 46 {
                this.ok = false
                return r
            }
            this.i += 1
            r.week = this.num()
            if this.peek() != 46 {
                this.ok = false
                return r
            }
            this.i += 1
            r.day = this.num()
        } else {
            r.kind = 1
            r.day = this.num()
        }
        if this.peek() == 47 {
            this.i += 1
            r.time = this.clock()
        }
        return r
    }
}

func parseTZRule(loc *Location, s string) {
    var sc = tzScan{s: s, ok: true}
    var stdName = sc.name()
    var stdOff = -sc.clock()
    if !sc.ok {
        return
    }
    loc.std = zone{name: stdName, offset: stdOff}
    loc.hasRule = true
    if !sc.more() {
        return
    }
    var dstName = sc.name()
    var dstOff = stdOff + 3600
    if sc.more() && sc.peek() != 44 {
        dstOff = -sc.clock()
    }
    if sc.peek() != 44 {
        return
    }
    sc.i += 1
    var start = sc.rule()
    if sc.peek() != 44 {
        return
    }
    sc.i += 1
    var end = sc.rule()
    if !sc.ok {
        return
    }
    loc.dst = zone{name: dstName, offset: dstOff, isDST: true}
    loc.start = start
    loc.end = end
    loc.hasDST = true
}

// ---- Time -------------------------------------------------------------------

// An instant with nanosecond precision and a location for display.
struct Time {
    sec  int
    nsec int
    loc  *Location

    // The current time in the local zone.
    static func Now() Time {
        var n = NowNanos()
        return Time{
            sec: floorDiv(n, 1000000000) + unixToInternal(),
            nsec: floorMod(n, 1000000000),
            loc: Local(),
        }
    }

    // Seconds since the Unix epoch.
    func Unix() int {
        return this.sec - unixToInternal()
    }

    func UnixMilli() int {
        return this.Unix() * 1000 + this.nsec / 1000000
    }

    func UnixMicro() int {
        return this.Unix() * 1000000 + this.nsec / 1000
    }

    // Nanoseconds since the Unix epoch (int64 range: years 1678..2262).
    func UnixNano() int {
        return this.Unix() * 1000000000 + this.nsec
    }

    // The nanosecond within the second, 0..999999999.
    func Nanosecond() int {
        return this.nsec
    }

    // True for the zero Time (January 1, year 1, 00:00:00 UTC).
    func IsZero() bool {
        return this.sec == 0 && this.nsec == 0
    }

    func location() *Location {
        if this.loc == nil {
            return UTC()
        }
        return this.loc
    }

    func zoneNow() zone {
        if this.loc == nil {
            return zone{name: "UTC", offset: 0}
        }
        return this.loc.lookup(this.Unix())
    }

    // Seconds since 1970 in local wall-clock terms.
    func localSec() int {
        return this.Unix() + this.zoneNow().offset
    }

    // (year, month 1..12, day 1..31) in the time's location.
    func Date() (int, int, int) {
        return civilFromDays(floorDiv(this.localSec(), 86400))
    }

    // (hour, minute, second) in the time's location.
    func Clock() (int, int, int) {
        var s = floorMod(this.localSec(), 86400)
        return (s / 3600, (s / 60) % 60, s % 60)
    }

    func Year() int {
        var (y, _, _) = this.Date()
        return y
    }

    func Month() int {
        var (_, m, _) = this.Date()
        return m
    }

    func MonthName() string {
        return monthNames()[this.Month() - 1]
    }

    func Day() int {
        var (_, _, d) = this.Date()
        return d
    }

    func Hour() int {
        return floorMod(this.localSec(), 86400) / 3600
    }

    func Minute() int {
        return floorMod(this.localSec(), 3600) / 60
    }

    func Second() int {
        return floorMod(this.localSec(), 60)
    }

    // 0 = Sunday ... 6 = Saturday.
    func Weekday() int {
        return floorMod(floorDiv(this.localSec(), 86400) + 4, 7)
    }

    func WeekdayName() string {
        return dayNames()[this.Weekday()]
    }

    // Day of the year, 1..366.
    func YearDay() int {
        var days = floorDiv(this.localSec(), 86400)
        var (y, _, _) = civilFromDays(days)
        return days - daysFromCivil(y, 1, 1) + 1
    }

    // ISO 8601 (year, week 1..53).
    func ISOWeek() (int, int) {
        var days = floorDiv(this.localSec(), 86400)
        // shift to the Thursday of this week (Monday-based)
        var wd = floorMod(days + 3, 7)
        var thu = days - wd + 3
        var (y, _, _) = civilFromDays(thu)
        return (y, (thu - daysFromCivil(y, 1, 1)) / 7 + 1)
    }

    // (zone abbreviation, offset in seconds east of UTC).
    func Zone() (string, int) {
        var z = this.zoneNow()
        return (z.name, z.offset)
    }

    func IsDST() bool {
        return this.zoneNow().isDST
    }

    func Location() *Location {
        return this.location()
    }

    // The same instant in UTC.
    func UTC() Time {
        return Time{sec: this.sec, nsec: this.nsec, loc: nil}
    }

    // The same instant in the local zone.
    func Local() Time {
        return Time{sec: this.sec, nsec: this.nsec, loc: Local()}
    }

    // The same instant in loc (nil: UTC).
    func In(loc *Location) Time {
        return Time{sec: this.sec, nsec: this.nsec, loc: loc}
    }

    func Add(d Duration) Time {
        var ns = this.nsec + d.ns % 1000000000
        var sec = this.sec + d.ns / 1000000000
        if ns >= 1000000000 {
            sec += 1
            ns -= 1000000000
        } else if ns < 0 {
            sec -= 1
            ns += 1000000000
        }
        return Time{sec: sec, nsec: ns, loc: this.loc}
    }

    // The duration this - u (saturating at the int64 range).
    func Sub(u Time) Duration {
        var ds = this.sec - u.sec
        if ds > 9223372035 {
            return Duration{ns: 9223372036854775807}
        }
        if ds < -9223372035 {
            return Duration{ns: -9223372036854775807 - 1}
        }
        return Duration{ns: ds * 1000000000 + (this.nsec - u.nsec)}
    }

    // Adds years, months and days in the time's location, normalizing
    // like Go (October 31 + 1 month = December 1).
    func AddDate(years int, months int, days int) Time {
        var (y, m, d) = this.Date()
        var (h, mi, s) = this.Clock()
        return Date(y + years, m + months, d + days, h, mi, s, this.nsec, this.loc)
    }

    func Before(u Time) bool {
        return this.sec < u.sec || (this.sec == u.sec && this.nsec < u.nsec)
    }

    func After(u Time) bool {
        return this.sec > u.sec || (this.sec == u.sec && this.nsec > u.nsec)
    }

    // Same instant (locations may differ).
    func Equal(u Time) bool {
        return this.sec == u.sec && this.nsec == u.nsec
    }

    func Equals(u Time) bool {
        return this.Equal(u)
    }

    func Compare(u Time) int {
        if this.Before(u) {
            return -1
        }
        if this.After(u) {
            return 1
        }
        return 0
    }

    // Rounded down to a multiple of d since the zero time (d <= 0: only
    // the monotonic... nothing to strip, unchanged).
    func Truncate(d Duration) Time {
        if d.ns <= 0 {
            return *this
        }
        var r = this.mod(d)
        return this.Add(Duration{ns: -r})
    }

    // Rounded to the nearest multiple of d since the zero time (halfway up).
    func Round(d Duration) Time {
        if d.ns <= 0 {
            return *this
        }
        var r = this.mod(d)
        if r + r < d.ns {
            return this.Add(Duration{ns: -r})
        }
        return this.Add(Duration{ns: d.ns - r})
    }

    // (time since the zero time) mod d, in ns.
    func mod(d Duration) int {
        if d.ns % 1000000000 == 0 {
            var ds = d.ns / 1000000000
            return floorMod(this.sec, ds) * 1000000000 + this.nsec
        }
        // sec * 1e9 + nsec mod d without overflow: (a*b + c) mod d
        var m = d.ns
        var s = floorMod(this.sec, m)
        var acc = 0
        // multiply s by 1e9 mod m by repeated doubling
        var mul = 1000000000
        var base = s
        for mul > 0 {
            if mul % 2 == 1 {
                acc = (acc + base) % m
            }
            base = (base + base) % m
            mul = mul / 2
        }
        return (acc + this.nsec) % m
    }

    // Formats with a Go reference layout (Mon Jan 2 15:04:05 MST 2006),
    // e.g. time.RFC3339 or "2006-01-02 15:04".
    func Format(layout string) string {
        return formatTime(*this, layout)
    }

    // Formats with strftime codes (%Y %m %d %H %M %S %y %b %B %a %A %j %e
    // %I %p %Z %z %s %L (ms) %f (µs) %N (ns) %F %T %D %%).
    func Strftime(layout string) string {
        return strftime(*this, layout)
    }

    // Go's default format: "2006-01-02 15:04:05.999999999 -0700 MST".
    func String() string {
        return this.Format("2006-01-02 15:04:05.999999999 -0700 MST")
    }
}

// The current time in the local zone (= Time.Now()).
func Current() Time {
    return Time.Now()
}

// The instant sec seconds + nsec nanoseconds after the Unix epoch, in UTC
// (nsec may be outside 0..1e9).
func Unix(sec int, nsec int) Time {
    var s = sec + floorDiv(nsec, 1000000000)
    return Time{sec: s + unixToInternal(), nsec: floorMod(nsec, 1000000000), loc: nil}
}

func UnixMilli(ms int) Time {
    return Unix(floorDiv(ms, 1000), floorMod(ms, 1000) * 1000000)
}

func UnixMicro(us int) Time {
    return Unix(floorDiv(us, 1000000), floorMod(us, 1000000) * 1000)
}

// The time of the wall clock y-m-d h:min:s.ns in loc (nil: UTC); out of
// range values normalize (month 13 = January next year). DST gaps and
// overlaps resolve exactly like Go's time.Date.
func Date(year int, month int, day int, hour int, min int, sec int, nsec int, loc *Location) Time {
    var m = month - 1
    var y = year + floorDiv(m, 12)
    m = floorMod(m, 12) + 1
    var s = sec + floorDiv(nsec, 1000000000)
    var ns = floorMod(nsec, 1000000000)
    var days = daysFromCivil(y, m, 1) + day - 1
    var local = days * 86400 + hour * 3600 + min * 60 + s
    var unix = local
    if loc != nil {
        // Go's Date: the zone at the wall time read as UTC, corrected
        // once if the resulting instant falls outside that zone.
        var z = loc.lookup(local)
        unix = local - z.offset
        var z2 = loc.lookup(unix)
        if z2.offset != z.offset {
            unix = local - z2.offset
        }
    }
    return Time{sec: unix + unixToInternal(), nsec: ns, loc: loc}
}

// Time since t (= Time.Now().Sub(t)).
func Elapsed(t Time) Duration {
    return Time.Now().Sub(t)
}

// Time until t (= t.Sub(Time.Now())).
func Until(t Time) Duration {
    return t.Sub(Time.Now())
}

// Blocks the thread for d (millisecond resolution); in async code use
// `await task.Delay(d.Milliseconds())`.
func SleepFor(d Duration) {
    if d.ns > 0 {
        Sleep((d.ns + 999999) / 1000000)
    }
}

// ---- Instant (monotonic) ----------------------------------------------------

// A reading of the monotonic clock, for measuring elapsed time (never
// jumps with wall-clock changes).
struct Instant {
    ns int

    func Elapsed() Duration {
        return Duration{ns: std.diag.rtMonoNanos() - this.ns}
    }

    func Sub(o Instant) Duration {
        return Duration{ns: this.ns - o.ns}
    }

    func Add(d Duration) Instant {
        return Instant{ns: this.ns + d.ns}
    }

    func Before(o Instant) bool {
        return this.ns < o.ns
    }
}

func NowInstant() Instant {
    return Instant{ns: std.diag.rtMonoNanos()}
}

// ---- Ticker / Timer (async) ---------------------------------------------------

// Fires every period: `for await t.Tick() { ... }`-style loops:
// `var t = time.NewTicker(time.Milliseconds(100)); for await t.Tick() {}`.
// Ticks keep a fixed cadence (missed ticks are dropped, as in Go). Stop
// makes the next Tick return false (after at most one period).
struct Ticker {
    st *tickState

    // Waits for the next tick; false once stopped.
    func Tick() Task<bool> {
        return tickerTick(*this)
    }

    func Stop() {
        this.st.stopped = true
    }

    // Changes the period (from now).
    func Reset(d Duration) {
        if d.ns <= 0 {
            panic("time: non-positive interval for Ticker.Reset")
        }
        this.st.period = d.ns
        this.st.next = std.diag.rtMonoNanos() + d.ns
        this.st.stopped = false
    }
}

struct tickState {
    period  int
    next    int
    stopped bool
}

// A ticker with period d (> 0).
func NewTicker(d Duration) Ticker {
    if d.ns <= 0 {
        panic("time: non-positive interval for NewTicker")
    }
    return Ticker{st: new tickState{period: d.ns, next: std.diag.rtMonoNanos() + d.ns}}
}

// Fires once after a duration: `await t.Wait()` is true when it fired,
// false if it was stopped first.
struct Timer {
    st *tickState

    func Wait() Task<bool> {
        return timerWait(*this)
    }

    // Prevents firing; true if the timer was still pending.
    func Stop() bool {
        var pending = !this.st.stopped
        this.st.stopped = true
        return pending
    }

    // Re-arms the timer to fire d from now; true if it was pending.
    func Reset(d Duration) bool {
        var pending = !this.st.stopped
        this.st.next = std.diag.rtMonoNanos() + d.ns
        this.st.stopped = false
        return pending
    }
}

func NewTimer(d Duration) Timer {
    return Timer{st: new tickState{period: d.ns, next: std.diag.rtMonoNanos() + d.ns}}
}

// Resolves after d with the time then (= task.Delay with a Duration).
async func After(d Duration) Time {
    if d.ns > 0 {
        await std.task.Delay((d.ns + 999999) / 1000000)
    }
    return Time.Now()
}

// ---- formatting ----------------------------------------------------------------

func monthNames() []string {
    return [
        "January",
        "February",
        "March",
        "April",
        "May",
        "June",
        "July",
        "August",
        "September",
        "October",
        "November",
        "December",
    ]
}

func dayNames() []string {
    return ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
}

func pad(n int, width int) string {
    var s = std.strconv.Itoa(n)
    var neg = n < 0
    if neg {
        s = s[1..]
    }
    for len(s) < width {
        s = "0" + s
    }
    if neg {
        return "-" + s
    }
    return s
}

func padSpace(n int, width int) string {
    var s = std.strconv.Itoa(n)
    for len(s) < width {
        s = " " + s
    }
    return s
}

// "+hh:mm" style offset text; colon chooses "+hh:mm" vs "+hhmm"; hourOnly "+hh".
func offsetText(off int, colon bool, hourOnly bool, seconds bool) string {
    var sign = "+"
    var o = off
    if o < 0 {
        sign = "-"
        o = -o
    }
    var out = sign + pad(o / 3600, 2)
    if hourOnly {
        return out
    }
    if colon {
        out += ":"
    }
    out += pad((o / 60) % 60, 2)
    if seconds {
        if colon {
            out += ":"
        }
        out += pad(o % 60, 2)
    }
    return out
}

// Layout chunk kinds (Go's std* constants, as strings for readability).
func nextChunk(layout string, i int) (string, int) {
    // Returns (token, length) of the std chunk starting at i, or ("", 0).
    var rest = layout[i..]
    var c = layout[i]
    switch c {
        case 74:
            if std.strings.HasPrefix(rest, "January") {
                return ("January", 7)
            }
            if std.strings.HasPrefix(rest, "Jan") {
                return ("Jan", 3)
            }
        case 77:
            if std.strings.HasPrefix(rest, "Monday") {
                return ("Monday", 6)
            }
            if std.strings.HasPrefix(rest, "Mon") {
                return ("Mon", 3)
            }
            if std.strings.HasPrefix(rest, "MST") {
                return ("MST", 3)
            }
        case 48:
            if len(rest) >= 2 && rest[1] >= 49 && rest[1] <= 54 {
                return (rest[..2], 2)
            }
            if std.strings.HasPrefix(rest, "002") {
                return ("002", 3)
            }
        case 49:
            if std.strings.HasPrefix(rest, "15") {
                return ("15", 2)
            }
            return ("1", 1)
        case 50:
            if std.strings.HasPrefix(rest, "2006") {
                return ("2006", 4)
            }
            return ("2", 1)
        case 95:
            if std.strings.HasPrefix(rest, "_2") {
                if std.strings.HasPrefix(rest, "_2006") {
                    return ("", 0)
                }
                return ("_2", 2)
            }
        case 51, 52, 53:
            return (rest[..1], 1)
        case 80:
            if std.strings.HasPrefix(rest, "PM") {
                return ("PM", 2)
            }
        case 112:
            if std.strings.HasPrefix(rest, "pm") {
                return ("pm", 2)
            }
        case 45:
            for tok in ["-07:00:00", "-0700", "-07:00", "-07"] {
                if std.strings.HasPrefix(rest, tok) {
                    return (tok, len(tok))
                }
            }
            if std.strings.HasPrefix(rest, "-070000") {
                return ("-070000", 7)
            }
        case 90:
            for tok in ["Z07:00:00", "Z070000", "Z0700", "Z07:00", "Z07"] {
                if std.strings.HasPrefix(rest, tok) {
                    return (tok, len(tok))
                }
            }
        case 46, 44:
            if len(rest) >= 2 && (rest[1] == 48 || rest[1] == 57) {
                var j = 1
                for j < len(rest) && rest[j] == rest[1] {
                    j += 1
                }
                // must not be followed by a digit
                if !(j < len(rest) && rest[j] >= 48 && rest[j] <= 57) {
                    return (rest[..j], j)
                }
            }
    }
    return ("", 0)
}

func formatTime(t Time, layout string) string {
    var (year, month, day) = t.Date()
    var (hour, min, sec) = t.Clock()
    var (zname, zoff) = t.Zone()
    var out = ""
    var i = 0
    for i < len(layout) {
        var (tok, n) = nextChunk(layout, i)
        if n == 0 {
            out += layout[i..i + 1]
            i += 1
            continue
        }
        i += n
        switch tok {
            case "2006": out += pad(year, 4)
            case "06": out += pad(floorMod(year, 100), 2)
            case "01": out += pad(month, 2)
            case "1": out += std.strconv.Itoa(month)
            case "January": out += monthNames()[month - 1]
            case "Jan": out += monthNames()[month - 1][..3]
            case "02": out += pad(day, 2)
            case "_2": out += padSpace(day, 2)
            case "2": out += std.strconv.Itoa(day)
            case "002": out += pad(t.YearDay(), 3)
            case "Monday": out += dayNames()[t.Weekday()]
            case "Mon": out += dayNames()[t.Weekday()][..3]
            case "15": out += pad(hour, 2)
            case "03": out += pad(hour12(hour), 2)
            case "3": out += std.strconv.Itoa(hour12(hour))
            case "04": out += pad(min, 2)
            case "4": out += std.strconv.Itoa(min)
            case "05": out += pad(sec, 2)
            case "5": out += std.strconv.Itoa(sec)
            case "PM":
                if hour >= 12 {
                    out += "PM"
                } else {
                    out += "AM"
                }
            case "pm":
                if hour >= 12 {
                    out += "pm"
                } else {
                    out += "am"
                }
            case "MST":
                if zname != "" {
                    out += zname
                } else {
                    out += offsetText(zoff, false, false, false)
                }
            case "-0700": out += offsetText(zoff, false, false, false)
            case "-07:00": out += offsetText(zoff, true, false, false)
            case "-07": out += offsetText(zoff, false, true, false)
            case "-070000": out += offsetText(zoff, false, false, true)
            case "-07:00:00": out += offsetText(zoff, true, false, true)
            case "Z0700", "Z07:00", "Z07", "Z070000", "Z07:00:00":
                if zoff == 0 {
                    out += "Z"
                } else {
                    out += offsetText(
                        zoff,
                        std.strings.Contains(tok, ":"),
                        tok == "Z07",
                        len(tok) > 6 && tok != "Z07:00",
                    )
                }
            default:
                // fractional seconds .000 / .999 (or ,000)
                var digits = len(tok) - 1
                var frac = pad(t.nsec, 9)[..digits]
                if tok[1] == 57 {
                    for std.strings.HasSuffix(frac, "0") {
                        frac = frac[..len(frac) - 1]
                    }
                    if frac != "" {
                        out += tok[..1] + frac
                    }
                } else {
                    out += tok[..1] + frac
                }
        }
    }
    return out
}

func hour12(h int) int {
    var x = h % 12
    if x == 0 {
        return 12
    }
    return x
}

func strftime(t Time, layout string) string {
    var (year, month, day) = t.Date()
    var (hour, min, sec) = t.Clock()
    var (zname, zoff) = t.Zone()
    var out = ""
    var i = 0
    for i < len(layout) {
        var c = layout[i]
        if c != 37 || i + 1 >= len(layout) {
            out += layout[i..i + 1]
            i += 1
            continue
        }
        var k = layout[i + 1]
        i += 2
        switch k {
            case 89: out += pad(year, 4)
            case 121: out += pad(floorMod(year, 100), 2)
            case 109: out += pad(month, 2)
            case 100: out += pad(day, 2)
            case 101: out += padSpace(day, 2)
            case 72: out += pad(hour, 2)
            case 73: out += pad(hour12(hour), 2)
            case 77: out += pad(min, 2)
            case 83: out += pad(sec, 2)
            case 76: out += pad(t.nsec / 1000000, 3)
            case 102: out += pad(t.nsec / 1000, 6)
            case 78: out += pad(t.nsec, 9)
            case 112:
                if hour >= 12 {
                    out += "PM"
                } else {
                    out += "AM"
                }
            case 98: out += monthNames()[month - 1][..3]
            case 66: out += monthNames()[month - 1]
            case 97: out += dayNames()[t.Weekday()][..3]
            case 65: out += dayNames()[t.Weekday()]
            case 106: out += pad(t.YearDay(), 3)
            case 117:
                var wd = t.Weekday()
                if wd == 0 {
                    wd = 7
                }
                out += std.strconv.Itoa(wd)
            case 119: out += std.strconv.Itoa(t.Weekday())
            case 90: out += zname
            case 122: out += offsetText(zoff, false, false, false)
            case 115: out += std.strconv.Itoa(t.Unix())
            case 70: out += pad(year, 4) + "-" + pad(month, 2) + "-" + pad(day, 2)
            case 84: out += pad(hour, 2) + ":" + pad(min, 2) + ":" + pad(sec, 2)
            case 68: out += pad(month, 2) + "/" + pad(day, 2) + "/" + pad(floorMod(year, 100), 2)
            case 110: out += "\n"
            case 116: out += "\t"
            case 37: out += "%"
            default:
                out += "%" + layout[i - 1..i]
        }
    }
    return out
}

// ---- parsing ---------------------------------------------------------------------

struct parser {
    v string
    i int

    func digits(min int, max int) int! {
        var start = this.i
        var n = 0
        for this.i < len(this.v) &&
            this.i - start < max &&
            this.v[this.i] >= 48 &&
            this.v[this.i] <= 57 {
            n = n * 10 + int(this.v[this.i]) - 48
            this.i += 1
        }
        if this.i - start < min {
            return error("bad number")
        }
        return n
    }
}

func parseErr(layout string, value string, what string) error {
    return error(
        "time: parsing time " +
            std.strconv.Quote(value) +
            " as " +
            std.strconv.Quote(layout) +
            ": " +
            what,
    )
}

func lookupName(names []string, v string, i int, short bool) int {
    for k, n in names {
        var name = n
        if short {
            name = n[..3]
        }
        if std.strings.HasPrefix(std.strings.ToLower(v[i..]), std.strings.ToLower(name)) {
            return k
        }
    }
    return -1
}

// Parses value with a Go reference layout, in UTC unless the value has a
// zone offset (Z07:00, -0700, ...) or names UTC/GMT.
func ParseTime(layout string, value string) Time! {
    return ParseInLocation(layout, value, nil)?
}

// Like ParseTime, interpreting zone-less values in loc (nil: UTC).
func ParseInLocation(layout string, value string, loc *Location) Time! {
    var p = parser{v: value}
    var year = 0
    var month = 1
    var day = 1
    var hour = 0
    var min = 0
    var sec = 0
    var nsec = 0
    var pm = -1
    var hasOff = false
    var off = 0
    var zname = ""
    var yday = -1
    var li = 0
    for li < len(layout) {
        var (tok, n) = nextChunk(layout, li)
        if n == 0 {
            if p.i >= len(value) || value[p.i] != layout[li] {
                return parseErr(layout, value, "mismatch at " + std.strconv.Quote(layout[li..]))
            }
            p.i += 1
            li += 1
            continue
        }
        li += n
        switch tok {
            case "2006":
                var neg = false
                if p.i < len(value) && value[p.i] == 45 {
                    neg = true
                    p.i += 1
                }
                year = try p.digits(4, 4) else return parseErr(layout, value, "bad year")
                if neg {
                    year = -year
                }
            case "06":
                year = try p.digits(2, 2) else return parseErr(layout, value, "bad year")
                if year >= 69 {
                    year += 1900
                } else {
                    year += 2000
                }
            case "01": month = try p.digits(2, 2) else return parseErr(layout, value, "bad month")
            case "1": month = try p.digits(1, 2) else return parseErr(layout, value, "bad month")
            case "January", "Jan":
                var k = lookupName(monthNames(), value, p.i, tok == "Jan")
                if k < 0 {
                    return parseErr(layout, value, "bad month name")
                }
                month = k + 1
                if tok == "Jan" {
                    p.i += 3
                } else {
                    p.i += len(monthNames()[k])
                }
            case "Monday", "Mon":
                var k = lookupName(dayNames(), value, p.i, tok == "Mon")
                if k < 0 {
                    return parseErr(layout, value, "bad day name")
                }
                if tok == "Mon" {
                    p.i += 3
                } else {
                    p.i += len(dayNames()[k])
                }
            case "02": day = try p.digits(2, 2) else return parseErr(layout, value, "bad day")
            case "_2":
                if p.i < len(value) && value[p.i] == 32 {
                    p.i += 1
                }
                day = try p.digits(1, 2) else return parseErr(layout, value, "bad day")
            case "2": day = try p.digits(1, 2) else return parseErr(layout, value, "bad day")
            case "002":
                yday = try p.digits(3, 3) else return parseErr(layout, value, "bad day of year")
            case "15": hour = try p.digits(1, 2) else return parseErr(layout, value, "bad hour")
            case "03", "3":
                hour = try p.digits(1, 2) else return parseErr(layout, value, "bad hour")
            case "04", "4":
                min = try p.digits(1, 2) else return parseErr(layout, value, "bad minute")
            case "05", "5":
                sec = try p.digits(1, 2) else return parseErr(layout, value, "bad second")
                // a fractional second may follow even if the layout has none
                if p.i + 1 < len(value) &&
                    value[p.i] == 46 &&
                    (li >= len(layout) || layout[li] != 46) &&
                    value[p.i + 1] >= 48 &&
                    value[p.i + 1] <= 57 {
                    p.i += 1
                    var start = p.i
                    var f = try p.digits(1, 9) else 0
                    var nd = p.i - start
                    for _ in nd..9 {
                        f = f * 10
                    }
                    nsec = f
                    for p.i < len(value) && value[p.i] >= 48 && value[p.i] <= 57 {
                        p.i += 1
                    }
                }
            case "PM", "pm":
                if p.i + 2 > len(value) {
                    return parseErr(layout, value, "bad AM/PM")
                }
                var ap = std.strings.ToUpper(value[p.i..p.i + 2])
                if ap == "PM" {
                    pm = 1
                } else if ap == "AM" {
                    pm = 0
                } else {
                    return parseErr(layout, value, "bad AM/PM")
                }
                p.i += 2
            case "MST":
                var start = p.i
                for p.i < len(value) &&
                    ((value[p.i] >= 65 && value[p.i] <= 90) ||
                        (value[p.i] >= 97 && value[p.i] <= 122)) {
                    p.i += 1
                }
                if p.i - start < 3 {
                    return parseErr(layout, value, "bad time zone")
                }
                zname = value[start..p.i]
            default:
                if tok[0] == 46 || tok[0] == 44 {
                    if p.i >= len(value) || (value[p.i] != 46 && value[p.i] != 44) {
                        if tok[1] == 57 {
                            continue
                        }
                        return parseErr(layout, value, "bad fractional second")
                    }
                    p.i += 1
                    var start = p.i
                    var f = try p.digits(1, 9) else return parseErr(
                        layout,
                        value,
                        "bad fractional second",
                    )
                    var nd = p.i - start
                    for _ in nd..9 {
                        f = f * 10
                    }
                    nsec = f
                    continue
                }
                // zone offsets
                if tok[0] == 90 && p.i < len(value) && value[p.i] == 90 {
                    p.i += 1
                    hasOff = true
                    off = 0
                    zname = "UTC"
                    continue
                }
                if p.i >= len(value) || (value[p.i] != 43 && value[p.i] != 45) {
                    return parseErr(layout, value, "bad zone offset")
                }
                var sign = 1
                if value[p.i] == 45 {
                    sign = -1
                }
                p.i += 1
                var hh = try p.digits(2, 2) else return parseErr(layout, value, "bad zone offset")
                var mm = 0
                var ss = 0
                if !std.strings.HasSuffix(tok, "07") {
                    if p.i < len(value) && value[p.i] == 58 {
                        p.i += 1
                    }
                    mm = try p.digits(2, 2) else return parseErr(layout, value, "bad zone offset")
                    if std.strings.HasSuffix(tok, "00:00") || std.strings.HasSuffix(tok, "0000") {
                        if p.i < len(value) && value[p.i] == 58 {
                            p.i += 1
                        }
                        ss = try p.digits(2, 2) else return parseErr(
                            layout,
                            value,
                            "bad zone offset",
                        )
                    }
                }
                hasOff = true
                off = sign * (hh * 3600 + mm * 60 + ss)
        }
    }
    if p.i != len(value) {
        return parseErr(layout, value, "extra text " + std.strconv.Quote(value[p.i..]))
    }
    if pm == 1 && hour < 12 {
        hour += 12
    } else if pm == 0 && hour == 12 {
        hour = 0
    }
    if yday >= 0 {
        var d = daysFromCivil(year, 1, 1) + yday - 1
        var (_, m2, d2) = civilFromDays(d)
        month = m2
        day = d2
    }
    if month < 1 || month > 12 {
        return parseErr(layout, value, "month out of range")
    }
    if day < 1 || day > daysIn(month, year) {
        return parseErr(layout, value, "day out of range")
    }
    if hour > 23 || min > 59 || sec > 59 {
        return parseErr(layout, value, "clock out of range")
    }
    if hasOff {
        var t = Date(year, month, day, hour, min, sec, nsec, nil).Add(Duration{
            ns: -off * 1000000000,
        })
        if off == 0 && (zname == "UTC" || zname == "") {
            return t
        }
        var name = zname
        if name == "" {
            name = offsetText(off, false, false, false)
        }
        return t.In(FixedZone(name, off))
    }
    if zname == "UTC" || zname == "GMT" {
        return Date(year, month, day, hour, min, sec, nsec, nil)
    }
    if zname != "" {
        // Go: an unknown abbreviation gets a fabricated zero-offset zone.
        var t = Date(year, month, day, hour, min, sec, nsec, loc)
        if loc != nil {
            var (n, _) = t.Zone()
            if n == zname {
                return t
            }
        }
        return Date(year, month, day, hour, min, sec, nsec, nil).In(FixedZone(zname, 0))
    }
    return Date(year, month, day, hour, min, sec, nsec, loc)
}

async func tickerTick(self Ticker) bool {
    var s = self.st
    if s.stopped {
        return false
    }
    var now = std.diag.rtMonoNanos()
    if s.next <= now {
        // drop missed ticks
        var missed = (now - s.next) / s.period + 1
        s.next += missed * s.period
    }
    var waitMs = (s.next - now + 999999) / 1000000
    await std.task.Delay(waitMs)
    if s.stopped {
        return false
    }
    s.next += s.period
    return true
}

async func timerWait(self Timer) bool {
    var s = self.st
    for !s.stopped {
        var now = std.diag.rtMonoNanos()
        if now >= s.next {
            s.stopped = true
            return true
        }
        // wake at most every 50 ms to notice Stop/Reset
        var waitMs = (s.next - now + 999999) / 1000000
        if waitMs > 50 {
            waitMs = 50
        }
        await std.task.Delay(waitMs)
    }
    return false
}
