namespace std.net.http.hpack

// HPACK (RFC 7541), the header compression of HTTP/2: the static table,
// a dynamic table per direction, integer and string literals, Huffman
// coding (the C helpers rtHuff* in runtime/src/std_http.c). Decoder and
// Encoder each keep the state of one direction of one connection.

// A header field. Sensitive fields are never indexed (encoded as
// "literal never indexed", and kept so by intermediaries).
struct HeaderField {
    Name      string
    Value     string
    Sensitive bool

    // The table size of the entry (RFC 7541 4.1).
    func Size() int {
        return len(this.Name) + len(this.Value) + 32
    }
}

// The dynamic table: entries[len-1] is the newest (index 62).
struct table {
    entries []HeaderField
    size    int
    maxSize int

    func add(f HeaderField) {
        var sz = f.Size()
        if sz > this.maxSize {
            this.entries = []
            this.size = 0
            return
        }
        for this.size + sz > this.maxSize {
            this.evict()
        }
        this.entries = append(this.entries, f)
        this.size += sz
    }

    func evict() {
        if len(this.entries) == 0 {
            return
        }
        this.size -= this.entries[0].Size()
        this.entries = this.entries[1..]
    }

    func setMax(n int) {
        this.maxSize = n
        for this.size > this.maxSize {
            this.evict()
        }
    }

    // Entry i of the combined index space (62 = newest dynamic entry).
    func at(i int) HeaderField! {
        if i >= 1 && i <= 61 {
            return HeaderField{Name: staticName(i), Value: staticValue(i)}
        }
        var d = i - 62
        if d < 0 || d >= len(this.entries) {
            return error("hpack: invalid table index " + std.fmt.Format("{0}", i))
        }
        return this.entries[len(this.entries) - 1 - d]
    }

    // The dynamic entry for name/value: index > 0 exact, -index name only, 0 none.
    func find(name string, value string) int {
        var nameIdx = 0
        var n = len(this.entries)
        var i = n - 1
        for i >= 0 {
            var e = this.entries[i]
            if e.Name == name {
                if e.Value == value {
                    return 62 + (n - 1 - i)
                }
                if nameIdx == 0 {
                    nameIdx = -(62 + (n - 1 - i))
                }
            }
            i -= 1
        }
        return nameIdx
    }
}

// ---- integers and strings (RFC 7541 5.1, 5.2) ----

// Appends the integer v with an n-bit prefix; first carries the bits above the prefix.
func appendInt(out []byte, first int, n int, v int) []byte {
    var max = (1 << n) - 1
    var b = out
    if v < max {
        b = append(b, byte(first | v))
        return b
    }
    b = append(b, byte(first | max))
    var x = v - max
    for x >= 128 {
        b = append(b, byte((x & 127) | 128))
        x = x >> 7
    }
    b = append(b, byte(x))
    return b
}

// Appends a string literal, Huffman-coded when that is shorter.
func appendString(out []byte, s string, huffman bool) []byte {
    var b = out
    if huffman {
        var hl = rtHuffLen(s)
        if hl < len(s) {
            b = appendInt(b, 128, 7, hl)
            for c in rtHuffEncode(s) {
                b = append(b, c)
            }
            return b
        }
    }
    b = appendInt(b, 0, 7, len(s))
    for i in 0..len(s) {
        b = append(b, s[i])
    }
    return b
}

// A decoding cursor over one header block.
struct cursor {
    b   []byte
    pos int

    func readInt(n int) int! {
        if this.pos >= len(this.b) {
            return error("hpack: truncated integer")
        }
        var max = (1 << n) - 1
        var v = int(this.b[this.pos]) & max
        this.pos += 1
        if v < max {
            return v
        }
        var shift = 0
        for {
            if this.pos >= len(this.b) {
                return error("hpack: truncated integer")
            }
            var c = int(this.b[this.pos])
            this.pos += 1
            v += (c & 127) << shift
            shift += 7
            if shift > 28 {
                return error("hpack: integer overflow")
            }
            if (c & 128) == 0 {
                return v
            }
        }
    }

    func readStr(limit int) string! {
        if this.pos >= len(this.b) {
            return error("hpack: truncated string")
        }
        var huff = (int(this.b[this.pos]) & 128) != 0
        var n = this.readInt(7)?
        if n > len(this.b) - this.pos {
            return error("hpack: truncated string")
        }
        if limit > 0 && n > limit {
            return error("hpack: string too long")
        }
        var s = ""
        if huff {
            s = rtHuffDecode(this.b, this.pos, n)?
        } else {
            s = std.strings.FromBytes(this.b[this.pos..this.pos + n])
        }
        this.pos += n
        return s
    }
}

// ---- decoder ----

// Decoder decodes the header blocks of one direction of a connection.
struct Decoder {
    t table
    // the largest table size the peer may select (our SETTINGS_HEADER_TABLE_SIZE)
    limit int
    // longest accepted string (0: no limit)
    MaxStringLength int

    // Decodes one complete header block (HEADERS + CONTINUATION payloads).
    func Decode(block []byte) []HeaderField! {
        var out []HeaderField = []
        var c = cursor{b: block}
        var fields = 0
        for c.pos < len(block) {
            var b = int(block[c.pos])
            if (b & 128) != 0 {
                var i = c.readInt(7)?
                if i == 0 {
                    return error("hpack: invalid index 0")
                }
                out = append(out, this.t.at(i)?)
            } else if (b & 192) == 64 {
                var f = this.literal(&c, 6)?
                this.t.add(f)
                out = append(out, f)
            } else if (b & 224) == 32 {
                if fields > 0 {
                    return error("hpack: dynamic table size update after a header field")
                }
                var n = c.readInt(5)?
                if n > this.limit {
                    return error("hpack: dynamic table size update above the limit")
                }
                this.t.setMax(n)
                continue
            } else {
                var f = this.literal(&c, 4)?
                f.Sensitive = (b & 240) == 16
                out = append(out, f)
            }
            fields += 1
        }
        return out
    }

    func literal(c *cursor, n int) HeaderField! {
        var idx = c.readInt(n)?
        var name = ""
        if idx == 0 {
            name = c.readStr(this.MaxStringLength)?
        } else {
            var e = this.t.at(idx)?
            name = e.Name
        }
        var value = c.readStr(this.MaxStringLength)?
        return HeaderField{Name: name, Value: value}
    }

    // Changes the limit announced in SETTINGS_HEADER_TABLE_SIZE.
    func SetMaxTableSize(n int) {
        this.limit = n
        if this.t.maxSize > n {
            this.t.setMax(n)
        }
    }

    // The current dynamic table size (bytes, RFC 7541 4.1) and entry count.
    func TableSize() int {
        return this.t.size
    }

    func TableLen() int {
        return len(this.t.entries)
    }
}

func NewDecoder(maxTableSize int) *Decoder {
    return new Decoder{t: table{maxSize: maxTableSize}, limit: maxTableSize}
}

// ---- encoder ----

// Encoder encodes the header blocks of one direction of a connection.
struct Encoder {
    t table
    // a pending dynamic table size update (after SetMaxTableSize)
    update  bool
    minSize int
    // Huffman-code strings when shorter (default on via NewEncoder)
    Huffman bool

    // Encodes fields into one header block.
    func Encode(fields []HeaderField) []byte {
        var out []byte = []
        if this.update {
            this.update = false
            if this.minSize < this.t.maxSize {
                out = appendInt(out, 32, 5, this.minSize)
            }
            out = appendInt(out, 32, 5, this.t.maxSize)
        }
        for f in fields {
            out = this.field(out, f)
        }
        return out
    }

    func field(out []byte, f HeaderField) []byte {
        var b = out
        var s = staticIndex(f.Name, f.Value)
        if s > 0 && !f.Sensitive {
            return appendInt(b, 128, 7, s)
        }
        var d = 0
        if !f.Sensitive {
            d = this.t.find(f.Name, f.Value)
            if d > 0 {
                return appendInt(b, 128, 7, d)
            }
        }
        var nameIdx = 0
        if s < 0 {
            nameIdx = -s
        } else if s > 0 {
            nameIdx = s
        } else if d < 0 {
            nameIdx = -d
        }
        if f.Sensitive {
            b = appendInt(b, 16, 4, nameIdx)
        } else if f.Size() > this.t.maxSize {
            b = appendInt(b, 0, 4, nameIdx)
        } else {
            b = appendInt(b, 64, 6, nameIdx)
            this.t.add(HeaderField{Name: f.Name, Value: f.Value})
        }
        if nameIdx == 0 {
            b = appendString(b, f.Name, this.Huffman)
        }
        return appendString(b, f.Value, this.Huffman)
    }

    // Changes the table size (the peer's SETTINGS_HEADER_TABLE_SIZE, or
    // less); the next block starts with a size update.
    func SetMaxTableSize(n int) {
        if n == this.t.maxSize {
            return
        }
        if !this.update || n < this.minSize {
            this.minSize = n
        }
        this.update = true
        this.t.setMax(n)
    }

    func TableSize() int {
        return this.t.size
    }

    func TableLen() int {
        return len(this.t.entries)
    }
}

func NewEncoder(maxTableSize int) *Encoder {
    return new Encoder{t: table{maxSize: maxTableSize}, Huffman: true}
}

// ---- the static table ----

// The static table (RFC 7541 appendix A): the name of entry i (1..61).
func staticName(i int) string {
    switch i {
        case 1: return ":authority"
        case 2: return ":method"
        case 3: return ":method"
        case 4: return ":path"
        case 5: return ":path"
        case 6: return ":scheme"
        case 7: return ":scheme"
        case 8: return ":status"
        case 9: return ":status"
        case 10: return ":status"
        case 11: return ":status"
        case 12: return ":status"
        case 13: return ":status"
        case 14: return ":status"
        case 15: return "accept-charset"
        case 16: return "accept-encoding"
        case 17: return "accept-language"
        case 18: return "accept-ranges"
        case 19: return "accept"
        case 20: return "access-control-allow-origin"
        case 21: return "age"
        case 22: return "allow"
        case 23: return "authorization"
        case 24: return "cache-control"
        case 25: return "content-disposition"
        case 26: return "content-encoding"
        case 27: return "content-language"
        case 28: return "content-length"
        case 29: return "content-location"
        case 30: return "content-range"
        case 31: return "content-type"
        case 32: return "cookie"
        case 33: return "date"
        case 34: return "etag"
        case 35: return "expect"
        case 36: return "expires"
        case 37: return "from"
        case 38: return "host"
        case 39: return "if-match"
        case 40: return "if-modified-since"
        case 41: return "if-none-match"
        case 42: return "if-range"
        case 43: return "if-unmodified-since"
        case 44: return "last-modified"
        case 45: return "link"
        case 46: return "location"
        case 47: return "max-forwards"
        case 48: return "proxy-authenticate"
        case 49: return "proxy-authorization"
        case 50: return "range"
        case 51: return "referer"
        case 52: return "refresh"
        case 53: return "retry-after"
        case 54: return "server"
        case 55: return "set-cookie"
        case 56: return "strict-transport-security"
        case 57: return "transfer-encoding"
        case 58: return "user-agent"
        case 59: return "vary"
        case 60: return "via"
        case 61: return "www-authenticate"
    }
    return ""
}

// The value of static entry i.
func staticValue(i int) string {
    switch i {
        case 2: return "GET"
        case 3: return "POST"
        case 4: return "/"
        case 5: return "/index.html"
        case 6: return "http"
        case 7: return "https"
        case 8: return "200"
        case 9: return "204"
        case 10: return "206"
        case 11: return "304"
        case 12: return "400"
        case 13: return "404"
        case 14: return "500"
        case 16: return "gzip, deflate"
    }
    return ""
}

// The static entry for name/value: i > 0 for an exact match, -i for a
// name match (the first entry of that name), 0 if the name is not there.
func staticIndex(name string, value string) int {
    switch name {
        case ":authority":
            if value == "" {
                return 1
            }
            return -1
        case ":method":
            switch value {
                case "GET": return 2
                case "POST": return 3
            }
            return -2
        case ":path":
            switch value {
                case "/": return 4
                case "/index.html": return 5
            }
            return -4
        case ":scheme":
            switch value {
                case "http": return 6
                case "https": return 7
            }
            return -6
        case ":status":
            switch value {
                case "200": return 8
                case "204": return 9
                case "206": return 10
                case "304": return 11
                case "400": return 12
                case "404": return 13
                case "500": return 14
            }
            return -8
        case "accept-charset":
            if value == "" {
                return 15
            }
            return -15
        case "accept-encoding":
            switch value {
                case "gzip, deflate": return 16
            }
            return -16
        case "accept-language":
            if value == "" {
                return 17
            }
            return -17
        case "accept-ranges":
            if value == "" {
                return 18
            }
            return -18
        case "accept":
            if value == "" {
                return 19
            }
            return -19
        case "access-control-allow-origin":
            if value == "" {
                return 20
            }
            return -20
        case "age":
            if value == "" {
                return 21
            }
            return -21
        case "allow":
            if value == "" {
                return 22
            }
            return -22
        case "authorization":
            if value == "" {
                return 23
            }
            return -23
        case "cache-control":
            if value == "" {
                return 24
            }
            return -24
        case "content-disposition":
            if value == "" {
                return 25
            }
            return -25
        case "content-encoding":
            if value == "" {
                return 26
            }
            return -26
        case "content-language":
            if value == "" {
                return 27
            }
            return -27
        case "content-length":
            if value == "" {
                return 28
            }
            return -28
        case "content-location":
            if value == "" {
                return 29
            }
            return -29
        case "content-range":
            if value == "" {
                return 30
            }
            return -30
        case "content-type":
            if value == "" {
                return 31
            }
            return -31
        case "cookie":
            if value == "" {
                return 32
            }
            return -32
        case "date":
            if value == "" {
                return 33
            }
            return -33
        case "etag":
            if value == "" {
                return 34
            }
            return -34
        case "expect":
            if value == "" {
                return 35
            }
            return -35
        case "expires":
            if value == "" {
                return 36
            }
            return -36
        case "from":
            if value == "" {
                return 37
            }
            return -37
        case "host":
            if value == "" {
                return 38
            }
            return -38
        case "if-match":
            if value == "" {
                return 39
            }
            return -39
        case "if-modified-since":
            if value == "" {
                return 40
            }
            return -40
        case "if-none-match":
            if value == "" {
                return 41
            }
            return -41
        case "if-range":
            if value == "" {
                return 42
            }
            return -42
        case "if-unmodified-since":
            if value == "" {
                return 43
            }
            return -43
        case "last-modified":
            if value == "" {
                return 44
            }
            return -44
        case "link":
            if value == "" {
                return 45
            }
            return -45
        case "location":
            if value == "" {
                return 46
            }
            return -46
        case "max-forwards":
            if value == "" {
                return 47
            }
            return -47
        case "proxy-authenticate":
            if value == "" {
                return 48
            }
            return -48
        case "proxy-authorization":
            if value == "" {
                return 49
            }
            return -49
        case "range":
            if value == "" {
                return 50
            }
            return -50
        case "referer":
            if value == "" {
                return 51
            }
            return -51
        case "refresh":
            if value == "" {
                return 52
            }
            return -52
        case "retry-after":
            if value == "" {
                return 53
            }
            return -53
        case "server":
            if value == "" {
                return 54
            }
            return -54
        case "set-cookie":
            if value == "" {
                return 55
            }
            return -55
        case "strict-transport-security":
            if value == "" {
                return 56
            }
            return -56
        case "transfer-encoding":
            if value == "" {
                return 57
            }
            return -57
        case "user-agent":
            if value == "" {
                return 58
            }
            return -58
        case "vary":
            if value == "" {
                return 59
            }
            return -59
        case "via":
            if value == "" {
                return 60
            }
            return -60
        case "www-authenticate":
            if value == "" {
                return 61
            }
            return -61
    }
    return 0
}
