// The parser of std.regex: RE2 syntax -> syntax tree (see regex.sl for the
// engine overview). The tree lives in an arena of `node`s addressed by
// index. Every character test is a rune class (sorted, merged [lo, hi]
// pairs): a literal is a one-rune class, `(?i)` literals are classes of
// their case-folding orbit, `.` is [0-0x10FFFF] minus '\n' unless (?s).
//
// Node ops: 0 empty, 1 class (r), 2 assertion (min: 1 BOL, 2 EOL, 4 BOT,
// 8 EOT, 16 word boundary, 32 not word boundary), 3 capture (subs[0],
// cap = group number), 4 concat (subs), 5 alternation (subs), 6 repeat
// (subs[0], min, max (-1 = unbounded), greedy).
// Parser flags: 1 (?i) fold case, 2 (?m) multi-line, 4 (?s) dot matches
// \n, 8 (?U) ungreedy.
namespace std.regex

struct node {
    op     int
    min    int
    max    int
    greedy bool
    r      []int
    subs   []int
    cap    int
}

struct parser {
    rs      []char
    offs    []int
    pos     int
    flags   int
    nodes   []node
    ncap    int
    names   []string
    pattern string

    func fail(msg string, at int) error {
        var off = len(this.pattern)
        if at >= 0 && at < len(this.offs) {
            off = this.offs[at]
        }
        return error(
            "regex: " +
                msg +
                " at offset " +
                std.fmt.Format("{0}", off) +
                " in " +
                std.encoding.json.Quote(this.pattern),
        )
    }

    func add(n node) int {
        this.nodes = append(this.nodes, n)
        return len(this.nodes) - 1
    }

    func more() bool {
        return this.pos < len(this.rs)
    }

    func peek() char {
        if this.pos < len(this.rs) {
            return this.rs[this.pos]
        }
        return char(0)
    }

    func peekAt(k int) char {
        if this.pos + k < len(this.rs) {
            return this.rs[this.pos + k]
        }
        return char(0)
    }

    func lookingAt(t string) bool {
        var i = 0
        for ch in t {
            if this.pos + i >= len(this.rs) || this.rs[this.pos + i] != ch {
                return false
            }
            i += 1
        }
        return true
    }

    // The text from rune index a to b, for messages.
    func text(a int, b int) string {
        var hi = b
        if hi > len(this.rs) {
            hi = len(this.rs)
        }
        return this.pattern[this.offs[a]..this.offs[hi]]
    }

    // alternation := concat ('|' concat)*
    func alt(depth int) int! {
        if depth > 1000 {
            return this.fail("expression nests too deeply", this.pos)
        }
        var subs []int = []
        subs = append(subs, this.concat(depth)?)
        for this.more() && this.peek() == '|' {
            this.pos += 1
            subs = append(subs, this.concat(depth)?)
        }
        if len(subs) == 1 {
            return subs[0]
        }
        return this.add(node{op: 5, subs: subs})
    }

    // concat := (atom quantifier*)*
    func concat(depth int) int! {
        var subs []int = []
        for this.more() {
            var c = this.peek()
            if c == '|' || c == ')' {
                break
            }
            if c == '*' || c == '+' || c == '?' {
                return this.fail("missing argument to repetition operator " + string(c), this.pos)
            }
            if c == '{' && this.repeatAhead() {
                return this.fail(
                    "missing argument to repetition operator " +
                        this.text(this.pos, this.repeatEnd()),
                    this.pos,
                )
            }
            if c == '\\' && this.peekAt(1) == 'Q' {
                // \Q...\E: literal text; a quantifier applies to its last rune
                this.pos += 2
                var lits []int = []
                for this.more() {
                    if this.lookingAt("\\E") {
                        this.pos += 2
                        break
                    }
                    lits = append(lits, this.literal(int(this.peek())))
                    this.pos += 1
                }
                if len(lits) == 0 {
                    continue
                }
                for k in 0..(len(lits) - 1) {
                    subs = append(subs, lits[k])
                }
                var lastLit = this.quantifiers(lits[len(lits) - 1], this.pos - 1)?
                subs = append(subs, lastLit)
                continue
            }
            var start = this.pos
            var a = this.atom(depth)?
            if a < 0 {
                // a flag group like (?i): no node
                continue
            }
            a = this.quantifiers(a, start)?
            subs = append(subs, a)
        }
        if len(subs) == 0 {
            return this.add(node{op: 0})
        }
        if len(subs) == 1 {
            return subs[0]
        }
        return this.add(node{op: 4, subs: subs})
    }

    // Applies the quantifiers after an atom (at most one, plus a lazy '?').
    func quantifiers(a int, start int) int! {
        var x = a
        var seen = false
        for this.more() {
            var c = this.peek()
            var opStart = this.pos
            var min = 0
            var max = -1
            if c == '*' {
                this.pos += 1
            } else if c == '+' {
                min = 1
                this.pos += 1
            } else if c == '?' {
                max = 1
                this.pos += 1
            } else if c == '{' && this.repeatAhead() {
                var end = this.repeatEnd()
                var r = this.parseRepeat()
                min = r[0]
                max = r[1]
                if min > 1000 || max > 1000 || (max >= 0 && min > max) {
                    return this.fail("invalid repeat count " + this.text(opStart, end), opStart)
                }
            } else {
                break
            }
            if seen {
                return this.fail(
                    "invalid nested repetition operator " + this.text(start, this.pos),
                    opStart,
                )
            }
            seen = true
            var greedy = (this.flags & 8) == 0
            if this.more() && this.peek() == '?' {
                this.pos += 1
                greedy = !greedy
            }
            x = this.add(node{op: 6, subs: [x], min: min, max: max, greedy: greedy})
        }
        return x
    }

    // True if the text at pos is a repetition {n}, {n,} or {n,m}.
    func repeatAhead() bool {
        return this.repeatEnd() > 0
    }

    // The rune index after a repetition at pos, or -1.
    func repeatEnd() int {
        var i = this.pos
        if i >= len(this.rs) || this.rs[i] != '{' {
            return -1
        }
        i += 1
        var digits = 0
        for i < len(this.rs) && this.rs[i] >= '0' && this.rs[i] <= '9' {
            i += 1
            digits += 1
        }
        if digits == 0 {
            return -1
        }
        if i < len(this.rs) && this.rs[i] == ',' {
            i += 1
            for i < len(this.rs) && this.rs[i] >= '0' && this.rs[i] <= '9' {
                i += 1
            }
        }
        if i < len(this.rs) && this.rs[i] == '}' {
            return i + 1
        }
        return -1
    }

    // Parses {n}, {n,} or {n,m} at pos (repeatAhead is true): [min, max].
    func parseRepeat() []int {
        this.pos += 1
        var min = this.number()
        var max = min
        if this.peek() == ',' {
            this.pos += 1
            if this.peek() == '}' {
                max = -1
            } else {
                max = this.number()
            }
        }
        this.pos += 1
        return [min, max]
    }

    func number() int {
        var n = 0
        for this.more() && this.peek() >= '0' && this.peek() <= '9' {
            if n < 100000 {
                n = n * 10 + int(this.peek()) - 48
            }
            this.pos += 1
        }
        return n
    }

    // One atom; -1 for a flag-setting group like (?i).
    func atom(depth int) int! {
        var c = this.peek()
        var start = this.pos
        if c == '(' {
            return this.group(depth)?
        }
        if c == '[' {
            var r = this.class()?
            return this.add(node{op: 1, r: r})
        }
        if c == '.' {
            this.pos += 1
            if (this.flags & 4) != 0 {
                return this.add(node{op: 1, r: [0, 1114111]})
            }
            return this.add(node{op: 1, r: [0, 9, 11, 1114111]})
        }
        if c == '^' {
            this.pos += 1
            if (this.flags & 2) != 0 {
                return this.add(node{op: 2, min: 1})
            }
            return this.add(node{op: 2, min: 4})
        }
        if c == '$' {
            this.pos += 1
            if (this.flags & 2) != 0 {
                return this.add(node{op: 2, min: 2})
            }
            return this.add(node{op: 2, min: 8})
        }
        if c == '\\' {
            if this.pos + 1 >= len(this.rs) {
                return this.fail("trailing backslash at end of expression", start)
            }
            var e = this.rs[this.pos + 1]
            if e == 'A' {
                this.pos += 2
                return this.add(node{op: 2, min: 4})
            }
            if e == 'z' {
                this.pos += 2
                return this.add(node{op: 2, min: 8})
            }
            if e == 'b' {
                this.pos += 2
                return this.add(node{op: 2, min: 16})
            }
            if e == 'B' {
                this.pos += 2
                return this.add(node{op: 2, min: 32})
            }
            if e == 'd' ||
                e == 'D' ||
                e == 'w' ||
                e == 'W' ||
                e == 's' ||
                e == 'S' ||
                e == 'p' ||
                e == 'P' {
                var r = this.perlOrUnicode()?
                return this.add(node{op: 1, r: r})
            }
            var ch = this.escape(false)?
            return this.literal(ch)
        }
        this.pos += 1
        return this.literal(int(c))
    }

    // A literal rune (a class of its folding orbit under (?i)).
    func literal(c int) int {
        if (this.flags & 1) != 0 {
            return this.add(node{op: 1, r: foldClass([c, c])})
        }
        return this.add(node{op: 1, r: [c, c]})
    }

    func group(depth int) int! {
        var start = this.pos
        this.pos += 1
        var saved = this.flags
        var cap = -1
        if this.peek() == '?' {
            if this.lookingAt("?=") ||
                this.lookingAt("?!") ||
                this.lookingAt("?<=") ||
                this.lookingAt("?<!") {
                var n = 2
                if this.peekAt(1) == '<' {
                    n = 3
                }
                return this.fail(
                    "lookaround is not supported (RE2 syntax): " + this.text(start, this.pos + n),
                    start,
                )
            }
            if this.lookingAt("?P=") || this.lookingAt("?P>") {
                return this.fail(
                    "backreferences are not supported (RE2 syntax): " +
                        this.text(start, this.pos + 3),
                    start,
                )
            }
            if this.lookingAt("?P<") || this.lookingAt("?<") {
                if this.lookingAt("?P<") {
                    this.pos += 3
                } else {
                    this.pos += 2
                }
                var nameStart = this.pos
                for this.more() && this.peek() != '>' {
                    this.pos += 1
                }
                if !this.more() {
                    return this.fail("invalid named capture " + this.text(start, this.pos), start)
                }
                var name = this.text(nameStart, this.pos)
                if !validName(name) {
                    return this.fail(
                        "invalid named capture " + this.text(start, this.pos + 1),
                        start,
                    )
                }
                this.pos += 1
                this.ncap += 1
                cap = this.ncap
                this.names = append(this.names, name)
            } else {
                // flags: (?flags) or (?flags:re)
                this.pos += 1
                var neg = false
                var any = false
                var f = this.flags
                for {
                    if !this.more() {
                        return this.fail("missing closing )", start)
                    }
                    var c = this.peek()
                    var bit = 0
                    if c == 'i' {
                        bit = 1
                    } else if c == 'm' {
                        bit = 2
                    } else if c == 's' {
                        bit = 4
                    } else if c == 'U' {
                        bit = 8
                    } else if c == '-' && !neg {
                        neg = true
                        any = false
                        this.pos += 1
                        continue
                    } else if (c == ':' || c == ')') && (any || !neg) {
                        break
                    } else {
                        return this.fail(
                            "invalid or unsupported Perl syntax " + this.text(start, this.pos + 1),
                            start,
                        )
                    }
                    if neg {
                        f = f & (15 - bit)
                    } else {
                        f = f | bit
                    }
                    any = true
                    this.pos += 1
                }
                if this.peek() == ')' {
                    // (?flags): applies to the rest of the enclosing group
                    if !any && !neg {
                        return this.fail("missing argument to repetition operator", start)
                    }
                    this.pos += 1
                    this.flags = f
                    return -1
                }
                this.pos += 1
                this.flags = f
                var inner = this.alt(depth + 1)?
                if !this.more() || this.peek() != ')' {
                    return this.fail("missing closing )", start)
                }
                this.pos += 1
                this.flags = saved
                return inner
            }
        } else {
            this.ncap += 1
            cap = this.ncap
            this.names = append(this.names, "")
        }
        var inner = this.alt(depth + 1)?
        if !this.more() || this.peek() != ')' {
            return this.fail("missing closing )", start)
        }
        this.pos += 1
        this.flags = saved
        return this.add(node{op: 3, subs: [inner], cap: cap})
    }

    // An escaped single character at pos (pos is at the backslash).
    func escape(inClass bool) int! {
        var start = this.pos
        this.pos += 1
        if !this.more() {
            return this.fail("trailing backslash at end of expression", start)
        }
        var c = this.peek()
        this.pos += 1
        if c >= '1' && c <= '7' && this.more() && this.peek() >= '0' && this.peek() <= '7' {
            // octal: \12 or \123
            var v = int(c) - 48
            for _ in 0..2 {
                if this.more() && this.peek() >= '0' && this.peek() <= '7' {
                    v = v * 8 + int(this.peek()) - 48
                    this.pos += 1
                }
            }
            return v
        }
        if c >= '1' && c <= '9' {
            return this.fail(
                "backreferences are not supported (RE2 syntax): " + this.text(start, this.pos),
                start,
            )
        }
        if c == '0' {
            var v = 0
            for _ in 0..2 {
                if this.more() && this.peek() >= '0' && this.peek() <= '7' {
                    v = v * 8 + int(this.peek()) - 48
                    this.pos += 1
                }
            }
            return v
        }
        if c == 'x' {
            if this.peek() == '{' {
                this.pos += 1
                var v = 0
                var digits = 0
                for this.more() && this.peek() != '}' {
                    var h = hexDigit(this.peek())
                    if h < 0 || v > 1114111 {
                        return this.fail(
                            "invalid escape sequence " + this.text(start, this.pos + 1),
                            start,
                        )
                    }
                    v = v * 16 + h
                    digits += 1
                    this.pos += 1
                }
                if !this.more() || digits == 0 || v > 1114111 {
                    return this.fail("invalid escape sequence " + this.text(start, this.pos), start)
                }
                this.pos += 1
                return v
            }
            var h1 = hexDigit(this.peek())
            var h2 = hexDigit(this.peekAt(1))
            if h1 < 0 || h2 < 0 {
                return this.fail("invalid escape sequence " + this.text(start, this.pos + 2), start)
            }
            this.pos += 2
            return h1 * 16 + h2
        }
        if c == 'a' {
            return 7
        }
        if c == 'f' {
            return 12
        }
        if c == 't' {
            return 9
        }
        if c == 'n' {
            return 10
        }
        if c == 'r' {
            return 13
        }
        if c == 'v' {
            return 11
        }
        if int(c) < 128 && !isAlnum(int(c)) {
            return int(c)
        }
        return this.fail("invalid escape sequence " + this.text(start, this.pos), start)
    }

    // \d \D \w \W \s \S \pX \p{Name} \PX \P{^Name} at pos (the backslash).
    func perlOrUnicode() []int! {
        var start = this.pos
        var e = this.rs[this.pos + 1]
        this.pos += 2
        var r []int = []
        if e == 'd' || e == 'D' {
            r = [48, 57]
        } else if e == 'w' || e == 'W' {
            r = [48, 57, 65, 90, 95, 95, 97, 122]
        } else if e == 's' || e == 'S' {
            r = [9, 10, 12, 13, 32, 32]
        } else {
            // \p / \P
            var neg = e == 'P'
            var name = ""
            if !this.more() {
                return this.fail(
                    "invalid character class range " + this.text(start, this.pos),
                    start,
                )
            }
            if this.peek() == '{' {
                var nameStart = this.pos + 1
                for this.more() && this.peek() != '}' {
                    this.pos += 1
                }
                if !this.more() {
                    return this.fail(
                        "invalid character class range " + this.text(start, this.pos),
                        start,
                    )
                }
                name = this.text(nameStart, this.pos)
                this.pos += 1
            } else {
                name = string(this.peek())
                this.pos += 1
            }
            if std.strings.HasPrefix(name, "^") {
                neg = !neg
                name = name[1..]
            }
            r = rtUnicode(name)
            if len(r) == 0 {
                return this.fail("unknown Unicode class " + this.text(start, this.pos), start)
            }
            if (this.flags & 1) != 0 {
                r = foldClass(r)
            }
            if neg {
                r = negate(r)
            }
            return r
        }
        if (this.flags & 1) != 0 {
            r = foldClass(r)
        }
        if e == 'D' || e == 'W' || e == 'S' {
            r = negate(r)
        }
        return r
    }

    // A bracketed class at pos ('[').
    func class() []int! {
        var start = this.pos
        this.pos += 1
        var neg = false
        if this.peek() == '^' {
            neg = true
            this.pos += 1
        }
        var r []int = []
        var first = true
        for {
            if !this.more() {
                return this.fail("missing closing ]", start)
            }
            var c = this.peek()
            if c == ']' && !first {
                this.pos += 1
                break
            }
            first = false
            if c == '[' && this.peekAt(1) == ':' {
                var end = this.posixEnd()
                if end > 0 {
                    var name = this.text(this.pos + 2, end - 2)
                    var pneg = false
                    if std.strings.HasPrefix(name, "^") {
                        pneg = true
                        name = name[1..]
                    }
                    var pr = posixClass(name)
                    if len(pr) == 0 {
                        return this.fail(
                            "invalid character class range " + this.text(this.pos, end),
                            this.pos,
                        )
                    }
                    if pneg {
                        pr = negate(pr)
                    }
                    r = appendRanges(r, pr)
                    this.pos = end
                    continue
                }
            }
            if c == '\\' && this.pos + 1 < len(this.rs) {
                var e = this.rs[this.pos + 1]
                if e == 'd' ||
                    e == 'D' ||
                    e == 'w' ||
                    e == 'W' ||
                    e == 's' ||
                    e == 'S' ||
                    e == 'p' ||
                    e == 'P' {
                    var saved = this.flags
                    this.flags = this.flags & 14
                    var pr = this.perlOrUnicode()?
                    this.flags = saved
                    r = appendRanges(r, pr)
                    continue
                }
            }
            var loStart = this.pos
            var lo = this.classChar()?
            var hi = lo
            if this.peek() == '-' && this.peekAt(1) != ']' && this.pos + 1 < len(this.rs) {
                this.pos += 1
                hi = this.classChar()?
                if hi < lo {
                    return this.fail(
                        "invalid character class range " + this.text(loStart, this.pos),
                        loStart,
                    )
                }
            }
            r = append(r, lo)
            r = append(r, hi)
        }
        r = normalize(r)
        if (this.flags & 1) != 0 {
            r = foldClass(r)
        }
        if neg {
            r = negate(r)
        }
        return r
    }

    // The rune index after a [:name:] at pos, or -1.
    func posixEnd() int {
        var i = this.pos + 2
        for i + 1 < len(this.rs) {
            if this.rs[i] == ':' && this.rs[i + 1] == ']' {
                return i + 2
            }
            if this.rs[i] == ']' {
                return -1
            }
            i += 1
        }
        return -1
    }

    func classChar() int! {
        var c = this.peek()
        if c == '\\' {
            if this.peekAt(1) == 'b' {
                return this.fail(
                    "invalid escape sequence " + this.text(this.pos, this.pos + 2),
                    this.pos,
                )
            }
            return this.escape(true)?
        }
        this.pos += 1
        return int(c)
    }
}

// Parses pattern into the arena of p; returns the root node.
func parse(pattern string, p *parser) int! {
    p.pattern = pattern
    p.rs = std.strings.Runes(pattern)
    p.names = [""]
    var offs []int = []
    var off = 0
    for i, ch in pattern {
        offs = append(offs, i)
        off = i + len(string(ch))
    }
    offs = append(offs, off)
    p.offs = offs
    var root = p.alt(0)?
    if p.more() {
        // only ')' stops the top-level alternation
        return p.fail("unexpected )", p.pos)
    }
    return root
}

func validName(name string) bool {
    if name == "" {
        return false
    }
    for ch in name {
        if !(ch == '_' ||
            (ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z')) {
            return false
        }
    }
    return true
}

func hexDigit(c char) int {
    if c >= '0' && c <= '9' {
        return int(c) - 48
    }
    if c >= 'a' && c <= 'f' {
        return int(c) - 87
    }
    if c >= 'A' && c <= 'F' {
        return int(c) - 55
    }
    return -1
}

func isAlnum(c int) bool {
    return (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)
}

func posixClass(name string) []int {
    if name == "alnum" {
        return [48, 57, 65, 90, 97, 122]
    }
    if name == "alpha" {
        return [65, 90, 97, 122]
    }
    if name == "ascii" {
        return [0, 127]
    }
    if name == "blank" {
        return [9, 9, 32, 32]
    }
    if name == "cntrl" {
        return [0, 31, 127, 127]
    }
    if name == "digit" {
        return [48, 57]
    }
    if name == "graph" {
        return [33, 126]
    }
    if name == "lower" {
        return [97, 122]
    }
    if name == "print" {
        return [32, 126]
    }
    if name == "punct" {
        return [33, 47, 58, 64, 91, 96, 123, 126]
    }
    if name == "space" {
        return [9, 13, 32, 32]
    }
    if name == "upper" {
        return [65, 90]
    }
    if name == "word" {
        return [48, 57, 65, 90, 95, 95, 97, 122]
    }
    if name == "xdigit" {
        return [48, 57, 65, 70, 97, 102]
    }
    var none []int = []
    return none
}

// -- rune classes ------------------------------------------------------------

func appendRanges(r []int, x []int) []int {
    var out = r
    for v in x {
        out = append(out, v)
    }
    return out
}

// Sorts [lo, hi] pairs by lo and merges overlapping or adjacent ones.
func normalize(r []int) []int {
    var n = len(r) / 2
    if n <= 1 {
        return r
    }
    var sorted = true
    for i in 1..n {
        if r[i * 2] < r[i * 2 - 2] {
            sorted = false
            break
        }
    }
    var src = r
    if !sorted {
        src = sortPairs(r)
    }
    var out []int = []
    var lo = src[0]
    var hi = src[1]
    for i in 1..n {
        var a = src[i * 2]
        var b = src[i * 2 + 1]
        if a <= hi + 1 {
            if b > hi {
                hi = b
            }
        } else {
            out = append(out, lo)
            out = append(out, hi)
            lo = a
            hi = b
        }
    }
    out = append(out, lo)
    out = append(out, hi)
    return out
}

// Merge sort of [lo, hi] pairs by lo.
func sortPairs(r []int) []int {
    var n = len(r) / 2
    var a = rtInts(n * 2)
    for i in 0..(n * 2) {
        a[i] = r[i]
    }
    var b = rtInts(n * 2)
    var width = 1
    for width < n {
        var i = 0
        for i < n {
            var mid = std.math.IMin(i + width, n)
            var hi = std.math.IMin(i + 2 * width, n)
            var x = i
            var y = mid
            var k = i
            for k < hi {
                if x < mid && (y >= hi || a[x * 2] <= a[y * 2]) {
                    b[k * 2] = a[x * 2]
                    b[k * 2 + 1] = a[x * 2 + 1]
                    x += 1
                } else {
                    b[k * 2] = a[y * 2]
                    b[k * 2 + 1] = a[y * 2 + 1]
                    y += 1
                }
                k += 1
            }
            i += 2 * width
        }
        var t = a
        a = b
        b = t
        width *= 2
    }
    return a
}

// Complement of a normalized class within [0, 0x10FFFF].
func negate(r0 []int) []int {
    var r = normalize(r0)
    var out []int = []
    var next = 0
    for i in 0..len(r) / 2 {
        if r[i * 2] > next {
            out = append(out, next)
            out = append(out, r[i * 2] - 1)
        }
        next = r[i * 2 + 1] + 1
    }
    if next <= 1114111 {
        out = append(out, next)
        out = append(out, 1114111)
    }
    return out
}

// Adds the simple case-folding orbits of every rune of the class.
func foldClass(r0 []int) []int {
    var r = normalize(r0)
    var out []int = []
    for v in r {
        out = append(out, v)
    }
    for i in 0..len(r) / 2 {
        var lo = r[i * 2]
        var hi = r[i * 2 + 1]
        // ASCII letters directly
        var a = std.math.IMax(lo, 65)
        var b = std.math.IMin(hi, 90)
        if a <= b {
            out = append(out, a + 32)
            out = append(out, b + 32)
        }
        a = std.math.IMax(lo, 97)
        b = std.math.IMin(hi, 122)
        if a <= b {
            out = append(out, a - 32)
            out = append(out, b - 32)
        }
        // 'k' / 's' fold to the Kelvin sign and the long s
        if lo <= 107 && hi >= 107 || lo <= 75 && hi >= 75 {
            out = append(out, 8490)
            out = append(out, 8490)
        }
        if lo <= 115 && hi >= 115 || lo <= 83 && hi >= 83 {
            out = append(out, 383)
            out = append(out, 383)
        }
        // other runes through the fold table (no fold beyond U+1E943)
        var c = std.math.IMax(lo, 128)
        var end = std.math.IMin(hi, 125251)
        for c <= end {
            var f = rtFold(c)
            for f != c {
                out = append(out, f)
                out = append(out, f)
                f = rtFold(f)
            }
            c += 1
        }
    }
    return normalize(out)
}
