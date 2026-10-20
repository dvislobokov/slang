namespace std.mime.multipart

// MIME multipart bodies (RFC 2046, RFC 7578 form-data), as Go's
// mime/multipart: a Writer that builds a body part by part, a Reader
// that walks the parts of a body, and ReadForm for form submissions.
// Parts with Content-Transfer-Encoding: quoted-printable are decoded.

// One part: its header and body.
struct Part {
    Header std.net.textproto.Header
    Body string

    // The name parameter of a form-data Content-Disposition ("" if none).
    func FormName() string {
        var cd = this.disposition()
        if cd.Type != "form-data" {
            return ""
        }
        return cd.Param("name")
    }

    // The filename parameter of the Content-Disposition (only its base name).
    func FileName() string {
        var f = this.disposition().Param("filename")
        if f == "" {
            return ""
        }
        var s = std.strings.Replace(f, "\\", "/")
        var slash = std.strings.LastIndex(s, "/")
        if slash >= 0 {
            return s[slash + 1..]
        }
        return s
    }

    func Bytes() []byte {
        return std.strings.Bytes(this.Body)
    }

    func disposition() std.mime.MediaType {
        var v = this.Header.Get("Content-Disposition")
        var m = try std.mime.ParseMediaType(v) else std.mime.MediaType{Params: map[string]string{}}
        return m
    }
}

// Reads the parts of a body with the given boundary.
struct Reader {
    body string
    boundary string
    pos int
    started bool
    done bool

    // The next part, or nil after the last one.
    func NextPart() *Part! {
        if this.done {
            return nil
        }
        var dash = "--" + this.boundary
        if !this.started {
            // the first delimiter may be at the very start or after a preamble
            var at = -1
            if std.strings.HasPrefix(this.body, dash) {
                at = 0
            } else {
                at = std.regex.rtIndex(this.body, "\n" + dash, 0)
                if at >= 0 {
                    at += 1
                }
            }
            if at < 0 {
                return error("multipart: NextPart: no boundary " + std.encoding.json.Quote(this.boundary) + " found")
            }
            this.pos = at
            this.started = true
        }
        // at a delimiter line
        var p = this.pos + len(dash)
        if std.strings.HasPrefix(this.body[p..], "--") {
            this.done = true
            return nil
        }
        // the rest of the delimiter line (transport padding) up to the line end
        var eol = std.regex.rtIndex(this.body, "\n", p)
        if eol < 0 {
            return error("multipart: unexpected end of body after a boundary")
        }
        if std.strings.Trim(this.body[p..eol]) != "" {
            return error("multipart: invalid boundary line")
        }
        var hr = std.net.textproto.ReadHeader(this.body, eol + 1)?
        var start = hr.End
        // the body ends at CRLF--boundary (or LF--boundary)
        var next = std.regex.rtIndex(this.body, "\n" + dash, start)
        if next < 0 {
            return error("multipart: unexpected end of body (no closing boundary)")
        }
        var end = next
        if end > start && int(this.body[end - 1]) == 13 {
            end -= 1
        }
        if next < start {
            end = start
        }
        var content = this.body[start..std.math.IMax(start, end)]
        this.pos = next + 1
        var h = hr.Header
        if std.strings.ToLower(h.Get("Content-Transfer-Encoding")) == "quoted-printable" {
            content = std.mime.quotedprintable.Decode(content)?
            h.Del("Content-Transfer-Encoding")
        }
        return &Part{Header: h, Body: content}
    }
}

func NewReader(body string, boundary string) *Reader {
    return &Reader{body: body, boundary: boundary}
}

// An uploaded file of a form.
struct FileHeader {
    Filename string
    Header std.net.textproto.Header
    Content []byte

    func Size() int {
        return len(this.Content)
    }
}

// A parsed multipart/form-data body.
struct Form {
    Value map[string][]string
    File map[string][]FileHeader

    // The first value of a field ("" if absent).
    func Get(name string) string {
        var vs = this.Value[name]
        if len(vs) == 0 {
            return ""
        }
        return vs[0]
    }
}

// Parses a form-data body: fields without a filename are values, the
// others files.
func ReadForm(body string, boundary string) Form! {
    var r = NewReader(body, boundary)
    var f = Form{Value: map[string][]string{}, File: map[string][]FileHeader{}}
    for {
        var p = r.NextPart()?
        if p == nil {
            break
        }
        var name = p.FormName()
        if name == "" {
            continue
        }
        var fname = p.FileName()
        if fname == "" && p.disposition().Param("filename") == "" {
            f.Value[name] = append(f.Value[name], p.Body)
        } else {
            f.File[name] = append(f.File[name], FileHeader{Filename: fname, Header: p.Header, Content: p.Bytes()})
        }
    }
    return f
}

// Builds a multipart body; call Close before taking the bytes.
struct Writer {
    boundary string
    b std.strings.Builder
    parts int
    closed bool

    func Boundary() string {
        return this.boundary
    }

    // Replaces the random boundary (1..70 characters of RFC 2046's set,
    // not ending in a space); only before the first part.
    func SetBoundary(b string) void! {
        if this.parts > 0 {
            return error("mime: SetBoundary called after write")
        }
        if len(b) < 1 || len(b) > 70 {
            return error("mime: invalid boundary length")
        }
        for i in 0..len(b) {
            var c = int(b[i])
            var ok = (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 39 || c == 40 || c == 41 || c == 43 || c == 95 || c == 44 || c == 45 || c == 46 || c == 47 || c == 58 || c == 61 || c == 63 || (c == 32 && i != len(b) - 1)
            if !ok {
                return error("mime: invalid boundary character")
            }
        }
        this.boundary = b
    }

    // The Content-Type of a form-data body with this boundary.
    func FormDataContentType() string {
        var b = this.boundary
        var special = false
        for i in 0..len(b) {
            var c = int(b[i])
            if c == 40 || c == 41 || c == 60 || c == 62 || c == 64 || c == 44 || c == 59 || c == 58 || c == 92 || c == 34 || c == 47 || c == 91 || c == 93 || c == 63 || c == 61 || c == 32 {
                special = true
            }
        }
        if special {
            b = "\"" + b + "\""
        }
        return "multipart/form-data; boundary=" + b
    }

    // Adds a part with the given header and body.
    func CreatePart(h std.net.textproto.Header, body []byte) void! {
        if this.closed {
            return error("multipart: write after Close")
        }
        if this.parts > 0 {
            this.b.Write("\r\n")
        }
        this.b.Write("--" + this.boundary + "\r\n")
        this.b.Write(h.String())
        this.b.Write("\r\n")
        this.b.Write(std.strings.FromBytes(body))
        this.parts += 1
    }

    // A form field.
    func WriteField(name string, value string) void! {
        var h = std.net.textproto.NewHeader()
        h.Set("Content-Disposition", "form-data; name=\"" + escapeQuotes(name) + "\"")
        this.CreatePart(h, std.strings.Bytes(value))?
    }

    // A file field (Content-Type application/octet-stream).
    func CreateFormFile(field string, filename string, content []byte) void! {
        var h = std.net.textproto.NewHeader()
        h.Set("Content-Disposition", "form-data; name=\"" + escapeQuotes(field) + "\"; filename=\"" + escapeQuotes(filename) + "\"")
        h.Set("Content-Type", "application/octet-stream")
        this.CreatePart(h, content)?
    }

    // Writes the closing boundary.
    func Close() {
        if this.closed {
            return
        }
        if this.parts > 0 {
            this.b.Write("\r\n")
        }
        this.b.Write("--" + this.boundary + "--\r\n")
        this.closed = true
    }

    func String() string {
        return this.b.String()
    }

    func Bytes() []byte {
        return std.strings.Bytes(this.b.String())
    }
}

// A writer with a random 60-hex-digit boundary.
func NewWriter() *Writer {
    var raw = try std.crypto.rand.Hex(30) else "0123456789abcdef0123456789abcdef0123456789abcdef0123456789ab"
    return &Writer{boundary: raw}
}

func escapeQuotes(s string) string {
    return std.strings.Replace(std.strings.Replace(s, "\\", "\\\\"), "\"", "\\\"")
}
