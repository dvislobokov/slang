namespace std.encoding.pem

// std.encoding.pem: PEM blocks (RFC 7468 / RFC 1421, Go's encoding/pem)
// with optional "Key: Value" headers. std.crypto.x509 keeps its own
// header-less PEM reader (x509.DecodePEM) for certificates.

// One block: -----BEGIN Type----- headers, base64 Bytes, -----END Type-----.
struct Block {
    Type string
    // Headers in order of appearance (RFC 1421 "Proc-Type: 4,ENCRYPTED").
    HeaderKeys []string
    HeaderValues []string
    Bytes []byte

    // The value of header key, or "".
    func Header(key string) string {
        for i, k in this.HeaderKeys {
            if k == key {
                return this.HeaderValues[i]
            }
        }
        return ""
    }

    // Adds or replaces a header.
    func SetHeader(key string, value string) {
        for i, k in this.HeaderKeys {
            if k == key {
                this.HeaderValues[i] = value
                return
            }
        }
        this.HeaderKeys = append(this.HeaderKeys, key)
        this.HeaderValues = append(this.HeaderValues, value)
    }
}

// The result of Decode.
struct Decoded {
    // The first block, or nil if there is none (or it is malformed).
    Block *Block
    // The text after that block (all of it when Block is nil).
    Rest string
}

// Finds the first well-formed PEM block in text, Go-style: text before it
// is skipped; on failure Block is nil and Rest is text.
func Decode(text string) Decoded {
    var rest = text
    for {
        var i = std.strings.Index(rest, "-----BEGIN ")
        if i < 0 {
            return Decoded{Block: nil, Rest: text}
        }
        rest = rest[i + 11..]
        var j = std.strings.Index(rest, "-----")
        if j < 0 {
            return Decoded{Block: nil, Rest: text}
        }
        var typ = rest[..j]
        if std.strings.Contains(typ, "\n") {
            continue
        }
        var after = rest[j + 5..]
        var endMark = "-----END " + typ + "-----"
        var k = std.strings.Index(after, endMark)
        if k < 0 {
            continue
        }
        var body = after[..k]
        var tail = after[k + len(endMark)..]
        if std.strings.HasPrefix(tail, "\r\n") {
            tail = tail[2..]
        } else if std.strings.HasPrefix(tail, "\n") {
            tail = tail[1..]
        }
        var b = new Block{Type: typ}
        var lines = std.strings.Split(std.strings.Replace(body, "\r", ""), "\n")
        var data = ""
        var inHeaders = true
        var bad = false
        for line in lines {
            var t = std.strings.Trim(line)
            if inHeaders {
                var colon = std.strings.Index(t, ":")
                if colon > 0 {
                    b.SetHeader(std.strings.Trim(t[..colon]), std.strings.Trim(t[colon + 1..]))
                    continue
                }
                if t == "" {
                    continue
                }
                inHeaders = false
            }
            if std.strings.Contains(t, ":") {
                bad = true
            }
            data += t
        }
        if bad {
            continue
        }
        var bytes = try std.encoding.base64.Decode(data) catch err {
            continue
        }
        b.Bytes = bytes
        return Decoded{Block: b, Rest: tail}
    }
}

// Every block in text, in order (malformed ones are skipped).
func DecodeAll(text string) []Block {
    var out []Block = []
    var rest = text
    for {
        var d = Decode(rest)
        if d.Block == nil {
            return out
        }
        out = append(out, *d.Block)
        rest = d.Rest
    }
}

// The PEM text of b: headers (sorted by key, Proc-Type first, as in Go),
// a blank line if there are headers, base64 in 64-column lines.
func Encode(b Block) string {
    var out = "-----BEGIN " + b.Type + "-----\n"
    if len(b.HeaderKeys) > 0 {
        var keys = std.slices.Clone(b.HeaderKeys)
        std.slices.Sort(keys)
        if b.Header("Proc-Type") != "" {
            out += "Proc-Type: " + b.Header("Proc-Type") + "\n"
        }
        for k in keys {
            if k != "Proc-Type" {
                out += k + ": " + b.Header(k) + "\n"
            }
        }
        out += "\n"
    }
    var b64 = std.encoding.base64.Encode(b.Bytes)
    var i = 0
    for i < len(b64) {
        var j = i + 64
        if j > len(b64) {
            j = len(b64)
        }
        out += b64[i..j] + "\n"
        i = j
    }
    return out + "-----END " + b.Type + "-----\n"
}
