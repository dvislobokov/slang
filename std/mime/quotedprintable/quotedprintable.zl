// Quoted-printable (RFC 2045 section 6.7), as Go's mime/quotedprintable:
// lines of at most 76 characters with soft breaks ("=\r\n"), "=XX" for
// '=' and non-printable bytes, trailing white space encoded, hard line
// breaks as \r\n (or encoded too in binary mode).
namespace std.mime.quotedprintable

// Encodes text data (its line breaks stay line breaks, written as \r\n).
func Encode(data string) string {
    return encode(data, false)
}

// Encodes binary data: \r and \n are encoded like other bytes.
func EncodeBinary(data string) string {
    return encode(data, true)
}

func hexDigit(v int) string {
    var d = "0123456789ABCDEF"
    return d[v..v + 1]
}

func encode(data string, binary bool) string {
    var out = std.strings.Builder{}
    var line = std.strings.Builder{}
    var n = len(data)
    var i = 0
    for i < n {
        var c = int(data[i])
        if !binary && (c == 10 || (c == 13 && i + 1 < n && int(data[i + 1]) == 10)) {
            // a hard line break: trailing white space of the line is encoded
            out.Write(trailing(line.String()))
            out.Write("\r\n")
            line.Reset()
            if c == 13 {
                i += 1
            }
            i += 1
            continue
        }
        var piece = ""
        if (c >= 33 && c <= 126 && c != 61) || c == 32 || c == 9 {
            piece = data[i..i + 1]
        } else {
            piece = "=" + hexDigit(c >> 4) + hexDigit(c & 15)
        }
        if line.Len() + len(piece) > 75 {
            out.Write(line.String())
            out.Write("=\r\n")
            line.Reset()
        }
        line.Write(piece)
        i += 1
    }
    out.Write(trailing(line.String()))
    return out.String()
}

// The line with a final space or tab encoded (it would be lost otherwise).
func trailing(l string) string {
    if l == "" {
        return l
    }
    var last = int(l[len(l) - 1])
    if last == 32 {
        return l[..len(l) - 1] + "=20"
    }
    if last == 9 {
        return l[..len(l) - 1] + "=09"
    }
    return l
}

func hexVal(c int) int {
    if c >= 48 && c <= 57 {
        return c - 48
    }
    if c >= 65 && c <= 70 {
        return c - 55
    }
    if c >= 97 && c <= 102 {
        return c - 87
    }
    return -1
}

// Decodes quoted-printable text: soft breaks are removed, white space at
// the end of encoded lines is dropped, line breaks are kept as written,
// and (as Go) a '=' not followed by two hex digits is kept literally.
func Decode(text string) string! {
    var out []byte = []
    var lines = std.strings.Split(text, "\n")
    for li, raw in lines {
        var line = raw
        var cr = std.strings.HasSuffix(line, "\r")
        if cr {
            line = line[..len(line) - 1]
        }
        line = std.strings.TrimRight(line, " \t")
        var soft = false
        var i = 0
        for i < len(line) {
            var c = int(line[i])
            if c == 61 {
                if i == len(line) - 1 {
                    soft = true
                    break
                }
                var h1 = hexVal(int(line[i + 1]))
                var h2 = -1
                if i + 2 < len(line) {
                    h2 = hexVal(int(line[i + 2]))
                }
                if h1 < 0 || h2 < 0 {
                    // as Go: an invalid escape is kept literally
                    out = append(out, line[i])
                    i += 1
                    continue
                }
                out = append(out, byte(h1 * 16 + h2))
                i += 3
                continue
            }
            out = append(out, line[i])
            i += 1
        }
        if li < len(lines) - 1 && !soft {
            if cr {
                out = append(out, byte(13))
            }
            out = append(out, byte(10))
        }
    }
    return std.strings.FromBytes(out)
}
