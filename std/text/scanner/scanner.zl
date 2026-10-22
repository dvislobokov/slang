// A tokenizer for Go-like source text (Go's text/scanner). Scan returns
// the next token: one of the negative token constants (EOF, Ident, Int,
// Float, Char, String, RawString, Comment) or the code of a single
// character. Mode selects what is recognized (GoTokens by default) and
// Whitespace the characters (< 64) that are skipped. Errors are collected
// in Errors as "line:col: message".
namespace std.text.scanner

struct Position {
    Filename string
    Offset   int
    Line     int
    Column   int

    func IsValid() bool {
        return this.Line > 0
    }

    func String() string {
        var s = this.Filename
        if this.IsValid() {
            if s != "" {
                s += ":"
            }
            s += std.fmt.Format("{0}:{1}", this.Line, this.Column)
        }
        if s == "" {
            s = "-"
        }
        return s
    }
}

struct Scanner {
    src string
    // offset of the next character
    off  int
    line int
    col  int
    // last read character (for Pos) and its width
    lastLineLen int
    ch          int
    tokStart    int
    tokEnd      int
    invalid     int

    Mode int
    // bit c set: character c (< 64) is skipped
    Whitespace int
    Filename   string
    // The position of the most recent token (set by Scan).
    Pos        Position
    Errors     []string
    ErrorCount int

    // Resets the scanner to the start of src (keeps Mode/Whitespace).
    func Init(src string) {
        this.src = src
        this.off = 0
        this.line = 1
        this.col = 1
        this.ch = -2
        this.tokStart = -1
        this.tokEnd = 0
        this.Errors = []
        this.ErrorCount = 0
        this.Pos = Position{}
    }

    // decodes the character at offset, -1 at the end
    func at(i int) (int, int) {
        if i >= len(this.src) {
            return (-1, 0)
        }
        var c = int(this.src[i])
        if c < 128 {
            return (c, 1)
        }
        var (r, n) = std.unicode.utf8.DecodeRuneAt(this.src, i)
        return (int(r), n)
    }

    // reads the next character
    func next() int {
        var (c, n) = this.at(this.off)
        if c < 0 {
            return -1
        }
        this.off += n
        if c == 10 {
            this.line += 1
            this.lastLineLen = this.col
            this.col = 1
        } else {
            this.col += 1
        }
        if c == 0 {
            this.error("invalid character NUL")
        }
        return c
    }

    func peekc() int {
        var (c, _) = this.at(this.off)
        return c
    }

    // Reads and returns the next character (EOF at the end).
    func Next() int {
        this.tokStart = -1
        this.Pos = Position{
            Filename: this.Filename,
            Offset: this.off,
            Line: this.line,
            Column: this.col,
        }
        return this.next()
    }

    // The next character without advancing.
    func Peek() int {
        return this.peekc()
    }

    func error(msg string) {
        this.ErrorCount += 1
        this.Errors = append(this.Errors, std.fmt.Format("{0}:{1}: {2}", this.line, this.col, msg))
    }

    func isIdentRune(c int, first bool) bool {
        if c == 95 || (c >= 97 && c <= 122) || (c >= 65 && c <= 90) {
            return true
        }
        if c >= 48 && c <= 57 {
            return !first
        }
        if c >= 128 {
            if std.unicode.IsLetter(char(c)) {
                return true
            }
            return !first && std.unicode.IsDigit(char(c))
        }
        return false
    }

    // Scans the next token; the token text is TokenText().
    func Scan() int {
        if this.line == 0 {
            this.Init(this.src)
        }
        for {
            var c = this.peekc()
            // skip white space
            for c >= 0 && c < 64 && (this.Whitespace >> c) & 1 == 1 {
                this.next()
                c = this.peekc()
            }
            this.tokStart = this.off
            this.Pos = Position{
                Filename: this.Filename,
                Offset: this.off,
                Line: this.line,
                Column: this.col,
            }
            if c < 0 {
                this.tokEnd = this.off
                return EOF
            }
            var tok = c
            if this.isIdentRune(c, true) && (this.Mode & ScanIdents) != 0 {
                this.next()
                for this.isIdentRune(this.peekc(), false) {
                    this.next()
                }
                tok = Ident
            } else if c >= 48 && c <= 57 && (this.Mode & (ScanInts | ScanFloats)) != 0 {
                tok = this.number(false)
            } else if c == 46 && (this.Mode & ScanFloats) != 0 && this.digitAfterDot() {
                tok = this.number(true)
            } else if c == 46 && (this.Mode & ScanFloats) != 0 {
                this.next()
            } else if c == 34 && (this.Mode & ScanStrings) != 0 {
                this.next()
                this.quoted(34)
                if this.peekc() == 34 {
                    this.next()
                }
                tok = String
            } else if c == 39 && (this.Mode & ScanChars) != 0 {
                this.next()
                this.charLit()
                tok = Char
            } else if c == 96 && (this.Mode & ScanRawStrings) != 0 {
                this.next()
                this.rawString()
                tok = RawString
            } else if c == 47 &&
                (this.Mode & ScanComments) != 0 &&
                (this.second() == 47 || this.second() == 42) {
                this.next()
                this.comment()
                if (this.Mode & SkipComments) != 0 {
                    continue
                }
                tok = Comment
            } else {
                this.next()
            }
            this.tokEnd = this.off
            return tok
        }
        return EOF
    }

    func second() int {
        var (_, n) = this.at(this.off)
        var (c, _) = this.at(this.off + n)
        return c
    }

    func digitAfterDot() bool {
        var c = this.second()
        return c >= 48 && c <= 57
    }

    // digits of base (any decimal digit for base <= 10; the first one out
    // of range is kept in invalid); bit 0: a digit, bit 1: a '_'
    func digits(base int) int {
        var digsep = 0
        for {
            var c = this.peekc()
            var d = digitVal(c)
            if c == 95 {
                digsep |= 2
            } else if (base <= 10 && d >= 0 && d < 10) || (base == 16 && d >= 0 && d < 16) {
                digsep |= 1
                if d >= base && this.invalid == 0 {
                    this.invalid = c
                }
            } else {
                return digsep
            }
            this.next()
        }
        return digsep
    }

    // Go's scanNumber (the first digit or '.' is not yet consumed)
    func number(dotFirst bool) int {
        var base = 10
        var prefix = 0
        var digsep = 0
        var tok = Float
        var seenDot = dotFirst
        this.invalid = 0
        if seenDot {
            this.next()
        } else {
            tok = Int
            if this.peekc() == 48 {
                this.next()
                var c = lower(this.peekc())
                if c == 120 {
                    this.next()
                    base = 16
                    prefix = 120
                } else if c == 111 {
                    this.next()
                    base = 8
                    prefix = 111
                } else if c == 98 {
                    this.next()
                    base = 2
                    prefix = 98
                } else {
                    base = 8
                    prefix = 48
                    digsep = 1
                }
            }
            digsep |= this.digits(base)
            if this.peekc() == 46 && (this.Mode & ScanFloats) != 0 {
                this.next()
                seenDot = true
            }
        }
        if seenDot {
            tok = Float
            if prefix == 111 || prefix == 98 {
                this.error("invalid radix point in " + litName(prefix))
            }
            digsep |= this.digits(base)
        }
        if digsep & 1 == 0 {
            this.error(litName(prefix) + " has no digits")
        }
        var e = lower(this.peekc())
        if (e == 101 || e == 112) && (this.Mode & ScanFloats) != 0 {
            if e == 101 && prefix != 0 && prefix != 48 {
                this.error(quoteChar(this.peekc()) + " exponent requires decimal mantissa")
            } else if e == 112 && prefix != 120 {
                this.error(quoteChar(this.peekc()) + " exponent requires hexadecimal mantissa")
            }
            this.next()
            tok = Float
            var sg = this.peekc()
            if sg == 43 || sg == 45 {
                this.next()
            }
            var ds = this.digits(10)
            digsep |= ds
            if ds & 1 == 0 {
                this.error("exponent has no digits")
            }
        } else if prefix == 120 && tok == Float {
            this.error("hexadecimal mantissa requires a 'p' exponent")
        }
        if tok == Int && this.invalid != 0 {
            this.error("invalid digit " + quoteChar(this.invalid) + " in " + litName(prefix))
        }
        if digsep & 2 != 0 && invalidSep(this.src[this.tokStart..this.off]) >= 0 {
            this.error("'_' must separate successive digits")
        }
        return tok
    }

    func escape(quote int) {
        var c = this.peekc()
        if c == quote ||
            c == 97 ||
            c == 98 ||
            c == 102 ||
            c == 110 ||
            c == 114 ||
            c == 116 ||
            c == 118 ||
            c == 92 {
            this.next()
            return
        }
        var n = 0
        var base = 0
        if c >= 48 && c <= 55 {
            this.next()
            n = 2
            base = 8
        } else if c == 120 {
            n = 2
            base = 16
        } else if c == 117 {
            n = 4
            base = 16
        } else if c == 85 {
            n = 8
            base = 16
        } else {
            if c < 0 {
                this.error("escape sequence not terminated")
            } else {
                this.error("invalid char escape")
            }
            return
        }
        if base == 16 {
            this.next()
        }
        for _ in 0..n {
            var d = this.peekc()
            if digitVal(d) < 0 || digitVal(d) >= base {
                if d < 0 {
                    this.error("escape sequence not terminated")
                } else {
                    this.error("invalid char escape")
                }
                return
            }
            this.next()
        }
    }

    func quoted(quote int) int {
        var n = 0
        for {
            var c = this.peekc()
            if c == quote {
                return n
            }
            if c == 10 || c < 0 {
                this.error("literal not terminated")
                return n
            }
            this.next()
            if c == 92 {
                this.escape(quote)
            }
            n += 1
        }
        return n
    }

    func charLit() {
        var n = this.quoted(39)
        if n != 1 {
            this.error("invalid char literal")
        }
        if this.peekc() == 39 {
            this.next()
        }
    }

    func rawString() {
        for {
            var c = this.next()
            if c == 96 {
                return
            }
            if c < 0 {
                this.error("literal not terminated")
                return
            }
        }
    }

    func comment() {
        var c = this.next()
        if c == 47 {
            // line comment (the newline is not part of it)
            for this.peekc() >= 0 && this.peekc() != 10 {
                this.next()
            }
            return
        }
        // general comment
        var prev = 0
        for {
            var d = this.next()
            if d < 0 {
                this.error("comment not terminated")
                return
            }
            if prev == 42 && d == 47 {
                return
            }
            prev = d
        }
    }

    // The text of the most recently scanned token.
    func TokenText() string {
        if this.tokStart < 0 {
            return ""
        }
        return this.src[this.tokStart..this.tokEnd]
    }

    // The position just after the last read character or token.
    func Position() Position {
        return Position{
            Filename: this.Filename,
            Offset: this.off,
            Line: this.line,
            Column: this.col,
        }
    }
}

// A scanner over src with Go tokens and Go white space.
func New(src string) *Scanner {
    var s = &Scanner{Mode: GoTokens, Whitespace: GoWhitespace}
    s.Init(src)
    return s
}

// A printable name of a token or character ("Ident", "EOF", `"+"`).
func TokenString(tok int) string {
    if tok == EOF {
        return "EOF"
    }
    if tok == Ident {
        return "Ident"
    }
    if tok == Int {
        return "Int"
    }
    if tok == Float {
        return "Float"
    }
    if tok == Char {
        return "Char"
    }
    if tok == String {
        return "String"
    }
    if tok == RawString {
        return "RawString"
    }
    if tok == Comment {
        return "Comment"
    }
    return std.encoding.json.Quote(std.strings.FromBytes(std.unicode.utf8.EncodeRune(char(tok))))
}

func digitVal(c int) int {
    if c >= 48 && c <= 57 {
        return c - 48
    }
    var l = lower(c)
    if l >= 97 && l <= 102 {
        return l - 87
    }
    if c >= 0 && c < 128 && (l >= 103 && l <= 122) {
        return 16
    }
    return -1
}

func lower(c int) int {
    if c >= 65 && c <= 90 {
        return c + 32
    }
    return c
}

func litName(prefix int) string {
    if prefix == 120 {
        return "hexadecimal literal"
    }
    if prefix == 111 || prefix == 48 {
        return "octal literal"
    }
    if prefix == 98 {
        return "binary literal"
    }
    return "decimal literal"
}

func quoteChar(c int) string {
    return "'" + std.strings.FromBytes(std.unicode.utf8.EncodeRune(char(c))) + "'"
}

// the index of a misplaced '_' in a number literal, or -1 (Go's invalidSep)
func invalidSep(x string) int {
    var x1 = 32
    var d = 46
    var i = 0
    if len(x) >= 2 && int(x[0]) == 48 {
        x1 = lower(int(x[1]))
        if x1 == 120 || x1 == 111 || x1 == 98 {
            d = 48
            i = 2
        }
    }
    for i < len(x) {
        var p = d
        d = int(x[i])
        var dv = digitVal(d)
        if d == 95 {
            if p != 48 {
                return i
            }
        } else if (dv >= 0 && dv < 10) || (x1 == 120 && dv >= 0 && dv < 16) {
            d = 48
        } else {
            if p == 95 {
                return i - 1
            }
            d = 46
        }
        i += 1
    }
    if d == 95 {
        return len(x) - 1
    }
    return -1
}
