namespace std.net.textproto

// Text protocols (SMTP, HTTP/1, MIME): MIME headers with canonical keys
// and continuation lines, dot-encoding of message bodies, and numeric
// response lines ("250-..."/"250 ...").

// A MIME header: keys in canonical form, several values per key, keys
// kept in first-seen order.
struct Header {
    keys []string
    vals [][]string

    // The first value of key ("" if absent).
    func Get(key string) string {
        var i = this.index(CanonicalKey(key))
        if i < 0 || len(this.vals[i]) == 0 {
            return ""
        }
        return this.vals[i][0]
    }

    // All values of key.
    func Values(key string) []string {
        var i = this.index(CanonicalKey(key))
        var out []string = []
        if i >= 0 {
            for v in this.vals[i] {
                out = append(out, v)
            }
        }
        return out
    }

    func Has(key string) bool {
        return this.index(CanonicalKey(key)) >= 0
    }

    // Replaces the values of key.
    func Set(key string, value string) {
        var k = CanonicalKey(key)
        var i = this.index(k)
        if i < 0 {
            this.keys = append(this.keys, k)
            this.vals = append(this.vals, [value])
            return
        }
        this.vals[i] = [value]
    }

    // Adds a value to key.
    func Add(key string, value string) {
        var k = CanonicalKey(key)
        var i = this.index(k)
        if i < 0 {
            this.keys = append(this.keys, k)
            this.vals = append(this.vals, [value])
            return
        }
        this.vals[i] = append(this.vals[i], value)
    }

    func Del(key string) {
        var i = this.index(CanonicalKey(key))
        if i < 0 {
            return
        }
        var ks []string = []
        var vs [][]string = []
        for j, k in this.keys {
            if j != i {
                ks = append(ks, k)
                vs = append(vs, this.vals[j])
            }
        }
        this.keys = ks
        this.vals = vs
    }

    // The keys in first-seen order.
    func Keys() []string {
        var out []string = []
        for k in this.keys {
            out = append(out, k)
        }
        return out
    }

    func Len() int {
        return len(this.keys)
    }

    // "Key: value\r\n" lines (keys sorted, as Go's Header.Write), without
    // the blank line that ends a header.
    func String() string {
        var ks = this.Keys()
        sortStrings(ks)
        var b = std.strings.Builder{}
        for k in ks {
            for v in this.vals[this.index(k)] {
                b.Write(k)
                b.Write(": ")
                b.Write(std.strings.Replace(std.strings.Replace(v, "\r", " "), "\n", " "))
                b.Write("\r\n")
            }
        }
        return b.String()
    }

    func index(k string) int {
        for i, x in this.keys {
            if x == k {
                return i
            }
        }
        return -1
    }
}

func NewHeader() Header {
    return Header{}
}

// The canonical form of a MIME header key: the first letter and each
// letter after a '-' upper case, the rest lower case ("content-type" ->
// "Content-Type"). Keys with spaces or other invalid bytes stay as they are.
func CanonicalKey(s string) string {
    var upper = true
    var b []byte = []
    for i in 0..len(s) {
        var c = int(s[i])
        if !tokenByte(c) {
            return s
        }
        if upper && c >= 97 && c <= 122 {
            c -= 32
        } else if !upper && c >= 65 && c <= 90 {
            c += 32
        }
        b = append(b, byte(c))
        upper = c == 45
    }
    return std.strings.FromBytes(b)
}

func tokenByte(c int) bool {
    if c <= 32 || c >= 127 {
        return false
    }
    // separators of RFC 7230 tokens
    return !(c == 40 || c == 41 || c == 60 || c == 62 || c == 64 || c == 44 || c == 59 || c == 58 || c == 92 || c == 34 || c == 47 || c == 91 || c == 93 || c == 63 || c == 61 || c == 123 || c == 125)
}

// A parsed header and where it ended.
struct HeaderResult {
    Header Header
    // Byte offset just after the blank line that ends the header.
    End int
}

// Reads a MIME header from text at offset `from`: "Key: value" lines
// (continuation lines starting with a space or tab are joined), ended by
// an empty line (\r\n or \n). A missing blank line at the end of text is
// accepted.
func ReadHeader(text string, from int) HeaderResult! {
    var h = Header{}
    var i = from
    var n = len(text)
    for i < n {
        var j = std.regex.rtIndex(text, "\n", i)
        var end = j
        var next = j + 1
        if j < 0 {
            end = n
            next = n
        }
        var line = text[i..end]
        if std.strings.HasSuffix(line, "\r") {
            line = line[..len(line) - 1]
        }
        if line == "" {
            return HeaderResult{Header: h, End: next}
        }
        var c0 = int(line[0])
        if c0 == 32 || c0 == 9 {
            if len(h.keys) == 0 {
                return error(std.fmt.Format("textproto: malformed MIME header initial line: {0}", std.encoding.json.Quote(line)))
            }
            // continuation of the last value
            var k = len(h.keys) - 1
            var vs = h.vals[k]
            vs[len(vs) - 1] = vs[len(vs) - 1] + " " + std.strings.Trim(line)
            i = next
            continue
        }
        var colon = std.strings.Index(line, ":")
        if colon <= 0 {
            return error(std.fmt.Format("textproto: malformed MIME header line: {0}", std.encoding.json.Quote(line)))
        }
        var key = line[..colon]
        if std.strings.HasSuffix(key, " ") || std.strings.HasSuffix(key, "\t") {
            return error(std.fmt.Format("textproto: malformed MIME header line: {0}", std.encoding.json.Quote(line)))
        }
        h.Add(key, std.strings.Trim(line[colon + 1..]))
        i = next
    }
    return HeaderResult{Header: h, End: n}
}

// Dot-encodes a message body (SMTP DATA): lines starting with '.' get a
// second '.', line ends become \r\n, and the ".\r\n" terminator is added.
func DotEncode(body string) string {
    var b = std.strings.Builder{}
    var lines = std.strings.Split(std.strings.Replace(body, "\r\n", "\n"), "\n")
    var last = len(lines) - 1
    if last >= 0 && lines[last] == "" {
        last -= 1
    }
    for k in 0..(last + 1) {
        var l = lines[k]
        if std.strings.HasPrefix(l, ".") {
            b.Write(".")
        }
        b.Write(l)
        b.Write("\r\n")
    }
    b.Write(".\r\n")
    return b.String()
}

// Decodes a dot-encoded body up to the "." line: the body (with \n line
// ends) and the offset after the terminator. Fails if there is none.
struct DotResult {
    Body string
    End int
}

func DotDecode(text string) DotResult! {
    var b = std.strings.Builder{}
    var i = 0
    var n = len(text)
    for i < n {
        var j = std.regex.rtIndex(text, "\n", i)
        if j < 0 {
            break
        }
        var line = text[i..j]
        if std.strings.HasSuffix(line, "\r") {
            line = line[..len(line) - 1]
        }
        if line == "." {
            return DotResult{Body: b.String(), End: j + 1}
        }
        if std.strings.HasPrefix(line, ".") {
            line = line[1..]
        }
        b.Write(line)
        b.Write("\n")
        i = j + 1
    }
    return error("textproto: unexpected end of dot-encoded data (no \".\" line)")
}

// A numeric response: code and the message (lines joined with \n).
struct Response {
    Code int
    Message string
    // Offset after the response.
    End int
}

// Reads a (possibly multi-line, "250-...") response at offset from; with
// expect > 0 the code must start with expect's digits (2 accepts 2xx).
func ReadResponse(text string, from int, expect int) Response! {
    var i = from
    var code = -1
    var msg []string = []
    for {
        var j = std.regex.rtIndex(text, "\n", i)
        if j < 0 {
            return error("textproto: unexpected end of response")
        }
        var line = text[i..j]
        if std.strings.HasSuffix(line, "\r") {
            line = line[..len(line) - 1]
        }
        i = j + 1
        if len(line) < 3 {
            return error("textproto: short response: " + std.encoding.json.Quote(line))
        }
        var c = try std.strings.ToInt(line[..3]) catch err {
            return error("textproto: invalid response code: " + std.encoding.json.Quote(line))
        }
        if code >= 0 && c != code {
            return error("textproto: inconsistent codes in a multi-line response")
        }
        code = c
        var cont = len(line) > 3 && int(line[3]) == 45
        if len(line) > 3 && !cont && int(line[3]) != 32 {
            return error("textproto: invalid response line: " + std.encoding.json.Quote(line))
        }
        if len(line) > 4 {
            msg = append(msg, line[4..])
        } else {
            msg = append(msg, "")
        }
        if !cont {
            break
        }
    }
    var r = Response{Code: code, Message: std.strings.Join(msg, "\n"), End: i}
    if expect > 0 {
        var digits = std.fmt.Format("{0}", expect)
        if !std.strings.HasPrefix(std.fmt.Format("{0}", code), digits) {
            return error(std.fmt.Format("{0} {1}", code, r.Message))
        }
    }
    return r
}

func sortStrings(a []string) {
    for i in 1..len(a) {
        var j = i
        for j > 0 && a[j - 1] > a[j] {
            var t = a[j]
            a[j] = a[j - 1]
            a[j - 1] = t
            j -= 1
        }
    }
}
