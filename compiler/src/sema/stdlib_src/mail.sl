namespace std.net.mail

// std.net.mail: RFC 5322 addresses and messages, MIME building (text,
// HTML alternatives, attachments, inline parts) and an SMTP client
// (RFC 5321) with STARTTLS (RFC 3207) and implicit TLS (SMTPS, port 465)
// over std.net.tls, and AUTH PLAIN, LOGIN and CRAM-MD5 (RFC 4954, 2195).
// DKIM signing is not implemented.

// ---- addresses --------------------------------------------------------------------

// A mailbox: "Name <user@example.com>".
struct Address {
    Name    string
    Address string

    // The RFC 5322 form: the name quoted or encoded (RFC 2047) when needed.
    func String() string {
        if this.Name == "" {
            return "<" + this.Address + ">"
        }
        return displayName(this.Name) + " <" + this.Address + ">"
    }
}

func isAtext(c int) bool {
    return (c >= 48 && c <= 57) ||
        (c >= 65 && c <= 90) ||
        (c >= 97 && c <= 122) ||
        c == 33 ||
        c == 35 ||
        c == 36 ||
        c == 37 ||
        c == 38 ||
        c == 39 ||
        c == 42 ||
        c == 43 ||
        c == 45 ||
        c == 47 ||
        c == 61 ||
        c == 63 ||
        c == 94 ||
        c == 95 ||
        c == 96 ||
        c == 123 ||
        c == 124 ||
        c == 125 ||
        c == 126
}

func displayName(name string) string {
    var ascii = true
    var atoms = true
    for i in 0..len(name) {
        var c = int(name[i])
        if c >= 128 || c < 32 {
            ascii = false
        }
        if !isAtext(c) && c != 32 {
            atoms = false
        }
    }
    if !ascii {
        return encodeWords(name)
    }
    if atoms {
        return name
    }
    var b = std.strings.Builder{}
    b.Write("\"")
    for i in 0..len(name) {
        var ch = name[i..i + 1]
        if ch == "\"" || ch == "\\" {
            b.Write("\\")
        }
        b.Write(ch)
    }
    b.Write("\"")
    return b.String()
}

func validAddr(a string) bool {
    var at = std.strings.LastIndex(a, "@")
    if at <= 0 || at == len(a) - 1 {
        return false
    }
    for i in 0..len(a) {
        var c = int(a[i])
        if c <= 32 || c == 60 || c == 62 || c == 44 || c == 59 || c == 127 {
            return false
        }
    }
    var domain = a[at + 1..]
    return !std.strings.HasPrefix(domain, ".") &&
        !std.strings.HasSuffix(domain, ".") &&
        !std.strings.Contains(domain, "..")
}

// Parses one address: "user@host", "<user@host>", "Name <user@host>",
// "\"Last, First\" <user@host>", "=?utf-8?q?...?= <user@host>", or the
// old "user@host (Name)" form.
func ParseAddress(s string) Address! {
    var list = ParseAddressList(s)?
    if len(list) != 1 {
        return error("mail: expected a single address in \"" + s + "\"")
    }
    return list[0]
}

// Parses a comma-separated address list (group syntax "Team: a@x, b@y;"
// is flattened).
func ParseAddressList(s string) []Address! {
    var out []Address = []
    var parts []string = []
    var cur = std.strings.Builder{}
    var quoted = false
    var angle = false
    var depth = 0
    var i = 0
    var n = len(s)
    for i < n {
        var c = s[i..i + 1]
        if quoted {
            cur.Write(c)
            if c == "\\" && i + 1 < n {
                cur.Write(s[i + 1..i + 2])
                i += 2
                continue
            }
            if c == "\"" {
                quoted = false
            }
        } else if c == "\"" {
            quoted = true
            cur.Write(c)
        } else if c == "(" {
            depth += 1
            cur.Write(c)
        } else if c == ")" {
            depth -= 1
            cur.Write(c)
        } else if c == "<" {
            angle = true
            cur.Write(c)
        } else if c == ">" {
            angle = false
            cur.Write(c)
        } else if (c == "," || c == ";") && !angle && depth == 0 {
            parts = append(parts, cur.String())
            cur.Reset()
        } else if c == ":" && !angle && depth == 0 && !std.strings.Contains(cur.String(), "@") {
            // group name: drop it
            cur.Reset()
        } else {
            cur.Write(c)
        }
        i += 1
    }
    if quoted {
        return error("mail: unterminated quoted string in \"" + s + "\"")
    }
    parts = append(parts, cur.String())
    for p in parts {
        var t = std.strings.Trim(p)
        if t == "" {
            continue
        }
        out = append(out, parseOne(t)?)
    }
    if len(out) == 0 {
        return error("mail: no address in \"" + s + "\"")
    }
    return out
}

func unquote(s string) string {
    var t = std.strings.Trim(s)
    if len(t) >= 2 && std.strings.HasPrefix(t, "\"") && std.strings.HasSuffix(t, "\"") {
        var b = std.strings.Builder{}
        var i = 1
        for i < len(t) - 1 {
            var c = t[i..i + 1]
            if c == "\\" && i + 1 < len(t) - 1 {
                i += 1
                c = t[i..i + 1]
            }
            b.Write(c)
            i += 1
        }
        return b.String()
    }
    return t
}

func parseOne(t string) Address! {
    var lt = std.strings.LastIndex(t, "<")
    if lt >= 0 {
        var gt = std.strings.LastIndex(t, ">")
        if gt < lt {
            return error("mail: missing '>' in \"" + t + "\"")
        }
        var addr = std.strings.Trim(t[lt + 1..gt])
        if !validAddr(addr) {
            return error("mail: invalid address \"" + addr + "\"")
        }
        var name = unquote(t[..lt])
        if std.strings.Contains(name, "=?") {
            name = std.mime.DecodeHeader(name)?
        }
        return Address{Name: name, Address: addr}
    }
    // "user@host (Name)"
    var op = std.strings.Index(t, "(")
    if op >= 0 && std.strings.HasSuffix(t, ")") {
        var addr = std.strings.Trim(t[..op])
        if !validAddr(addr) {
            return error("mail: invalid address \"" + addr + "\"")
        }
        return Address{Name: std.strings.Trim(t[op + 1..len(t) - 1]), Address: addr}
    }
    if !validAddr(t) {
        return error("mail: invalid address \"" + t + "\"")
    }
    return Address{Address: t}
}

// ---- header encoding --------------------------------------------------------------

// RFC 2047 Q-encoded words of at most 75 characters (split on UTF-8
// sequence boundaries), separated by folding white space.
func encodeWords(s string) string {
    var words []string = []
    var cur = std.strings.Builder{}
    var curLen = 0
    var i = 0
    var n = len(s)
    for i < n {
        // one UTF-8 sequence
        var c = int(s[i])
        var k = 1
        if c >= 240 {
            k = 4
        } else if c >= 224 {
            k = 3
        } else if c >= 192 {
            k = 2
        }
        if i + k > n {
            k = n - i
        }
        var enc = std.strings.Builder{}
        for j in i..i + k {
            var b = int(s[j])
            if b == 32 {
                enc.Write("_")
            } else if (b >= 48 && b <= 57) ||
                (b >= 65 && b <= 90) ||
                (b >= 97 && b <= 122) ||
                b == 33 ||
                b == 42 ||
                b == 43 ||
                b == 45 ||
                b == 47 {
                enc.Write(s[j..j + 1])
            } else {
                var d = "0123456789ABCDEF"
                enc.Write("=" + d[b >> 4..(b >> 4) + 1] + d[b & 15..(b & 15) + 1])
            }
        }
        var e = enc.String()
        if curLen + len(e) > 75 - 12 {
            words = append(words, "=?utf-8?q?" + cur.String() + "?=")
            cur.Reset()
            curLen = 0
        }
        cur.Write(e)
        curLen += len(e)
        i += k
    }
    if curLen > 0 || len(words) == 0 {
        words = append(words, "=?utf-8?q?" + cur.String() + "?=")
    }
    return std.strings.Join(words, "\r\n ")
}

func needsEncoding(s string) bool {
    for i in 0..len(s) {
        var c = int(s[i])
        if c >= 128 || (c < 32 && c != 9) {
            return true
        }
    }
    return false
}

// Folds a header line "Name: value" at white space so that lines stay
// within 78 characters where possible (RFC 5322 2.2.3).
func FoldHeader(name string, value string) string {
    var line = name + ": "
    var out = std.strings.Builder{}
    var lineLen = len(line)
    out.Write(line)
    var words = std.strings.Split(value, " ")
    for i, w in words {
        if std.strings.Contains(w, "\r\n") {
            // already folded (encoded words)
            out.Write(w)
            lineLen = len(w) - std.strings.LastIndex(w, "\n")
            continue
        }
        if i > 0 {
            if lineLen + 1 + len(w) > 78 {
                out.Write("\r\n ")
                lineLen = 1
            } else {
                out.Write(" ")
                lineLen += 1
            }
        }
        out.Write(w)
        lineLen += len(w)
    }
    out.Write("\r\n")
    return out.String()
}

// ---- messages ---------------------------------------------------------------------

// A file attached to a message. Inline parts (with ContentID) can be
// referenced from the HTML body as "cid:<ContentID>".
struct Attachment {
    Filename string
    // Default: by the file extension (std.mime), else application/octet-stream.
    ContentType string
    Data        []byte
    Inline      bool
    ContentID   string
}

// An email message. Build produces the RFC 5322 text: multipart/mixed
// for attachments, multipart/alternative for Text + HTML,
// multipart/related for inline parts; text parts are quoted-printable,
// attachments base64.
struct Message {
    From Address
    To   []Address
    Cc   []Address
    // Envelope recipients only (no header).
    Bcc         []Address
    ReplyTo     []Address
    Subject     string
    Text        string
    HTML        string
    Attachments []Attachment
    // Extra headers ("X-Mailer", "List-Unsubscribe", ...).
    Headers map[string]string
    // Default: now.
    Date std.time.Time
    // Default: a random <...@sender-domain>.
    MessageID string
    // Fixed MIME boundaries for reproducible output (tests); default random.
    Boundary string

    // All envelope recipients (To, Cc, Bcc).
    func Recipients() []string {
        var out []string = []
        for a in this.To {
            out = append(out, a.Address)
        }
        for a in this.Cc {
            out = append(out, a.Address)
        }
        for a in this.Bcc {
            out = append(out, a.Address)
        }
        return out
    }

    func Build() string! {
        if this.From.Address == "" {
            return error("mail: message has no From address")
        }
        if len(this.To) + len(this.Cc) + len(this.Bcc) == 0 {
            return error("mail: message has no recipients")
        }
        var b = std.strings.Builder{}
        var date = this.Date
        if date.IsZero() {
            date = std.time.Time.Now()
        }
        b.Write("Date: " + date.Format("Mon, 02 Jan 2006 15:04:05 -0700") + "\r\n")
        b.Write(FoldHeader("From", this.From.String()))
        if len(this.To) > 0 {
            b.Write(FoldHeader("To", joinAddrs(this.To)))
        }
        if len(this.Cc) > 0 {
            b.Write(FoldHeader("Cc", joinAddrs(this.Cc)))
        }
        if len(this.ReplyTo) > 0 {
            b.Write(FoldHeader("Reply-To", joinAddrs(this.ReplyTo)))
        }
        var subject = this.Subject
        if needsEncoding(subject) {
            subject = encodeWords(subject)
        }
        b.Write(FoldHeader("Subject", subject))
        var mid = this.MessageID
        if mid == "" {
            var at = std.strings.LastIndex(this.From.Address, "@")
            mid = "<" +
                std.encoding.hex.Encode(std.crypto.rand.Bytes(12)?) +
                "@" +
                this.From.Address[at + 1..] +
                ">"
        }
        b.Write("Message-ID: " + mid + "\r\n")
        if this.Headers != nil {
            var keys []string = []
            for k, _ in this.Headers {
                keys = append(keys, k)
            }
            sortStrings(keys)
            for k in keys {
                var v = this.Headers[k]
                if needsEncoding(v) {
                    v = encodeWords(v)
                }
                b.Write(FoldHeader(std.net.textproto.CanonicalKey(k), v))
            }
        }
        b.Write("MIME-Version: 1.0\r\n")
        var seq = 0
        var inline []Attachment = []
        var files []Attachment = []
        for a in this.Attachments {
            if a.Inline {
                inline = append(inline, a)
            } else {
                files = append(files, a)
            }
        }
        var body = this.alternative(&seq, inline)?
        if len(files) > 0 {
            var bd = this.boundary(&seq)?
            var mb = std.strings.Builder{}
            mb.Write("Content-Type: multipart/mixed; boundary=\"" + bd + "\"\r\n\r\n")
            mb.Write("--" + bd + "\r\n")
            mb.Write(body)
            for a in files {
                mb.Write("\r\n--" + bd + "\r\n")
                mb.Write(attachmentPart(a))
            }
            mb.Write("\r\n--" + bd + "--\r\n")
            body = mb.String()
        }
        b.Write(body)
        return b.String()
    }

    func boundary(seq *int) string! {
        *seq = *seq + 1
        if this.Boundary != "" {
            return this.Boundary + "_" + std.fmt.Format("{0}", *seq)
        }
        return "=_slang_" + std.encoding.hex.Encode(std.crypto.rand.Bytes(12)?)
    }

    // The text/HTML part (with inline parts): headers + body.
    func alternative(seq *int, inline []Attachment) string! {
        var text = textPart("text/plain", this.Text)
        if this.HTML == "" {
            return text
        }
        var html = textPart("text/html", this.HTML)
        if len(inline) > 0 {
            var rb = this.boundary(seq)?
            var r = std.strings.Builder{}
            r.Write("Content-Type: multipart/related; boundary=\"" + rb + "\"\r\n\r\n")
            r.Write("--" + rb + "\r\n" + html)
            for a in inline {
                r.Write("\r\n--" + rb + "\r\n" + attachmentPart(a))
            }
            r.Write("\r\n--" + rb + "--\r\n")
            html = r.String()
        }
        if this.Text == "" {
            return html
        }
        var ab = this.boundary(seq)?
        var m = std.strings.Builder{}
        m.Write("Content-Type: multipart/alternative; boundary=\"" + ab + "\"\r\n\r\n")
        m.Write("--" + ab + "\r\n" + text)
        m.Write("\r\n--" + ab + "\r\n" + html)
        m.Write("\r\n--" + ab + "--\r\n")
        return m.String()
    }
}

func joinAddrs(list []Address) string {
    var parts []string = []
    for a in list {
        parts = append(parts, a.String())
    }
    return std.strings.Join(parts, ", ")
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

func textPart(ctype string, body string) string {
    var plain = true
    var longLine = false
    var run = 0
    for i in 0..len(body) {
        var c = int(body[i])
        if c >= 128 || (c < 32 && c != 10 && c != 13 && c != 9) {
            plain = false
        }
        if c == 10 {
            run = 0
        } else {
            run += 1
            if run > 76 {
                longLine = true
            }
        }
    }
    if plain && !longLine {
        return "Content-Type: " +
            ctype +
            "; charset=us-ascii\r\nContent-Transfer-Encoding: 7bit\r\n\r\n" +
            crlf(body)
    }
    return "Content-Type: " +
        ctype +
        "; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n" +
        std.mime.quotedprintable.Encode(body)
}

func crlf(s string) string {
    return std.strings.Replace(std.strings.Replace(s, "\r\n", "\n"), "\n", "\r\n")
}

func attachmentPart(a Attachment) string {
    var ct = a.ContentType
    if ct == "" {
        var dot = std.strings.LastIndex(a.Filename, ".")
        if dot >= 0 {
            ct = std.mime.TypeByExtension(a.Filename[dot..])
        }
        if ct == "" {
            ct = "application/octet-stream"
        }
    }
    var name = a.Filename
    var nameParam = "\"" + name + "\""
    if needsEncoding(name) || std.strings.Contains(name, "\"") {
        nameParam = "\"" + encodeWords(name) + "\""
    }
    var b = std.strings.Builder{}
    b.Write("Content-Type: " + ct + "; name=" + nameParam + "\r\n")
    var disp = "attachment"
    if a.Inline {
        disp = "inline"
    }
    b.Write("Content-Disposition: " + disp + "; filename=" + nameParam + "\r\n")
    if a.ContentID != "" {
        b.Write("Content-ID: <" + a.ContentID + ">\r\n")
    }
    b.Write("Content-Transfer-Encoding: base64\r\n\r\n")
    var enc = std.encoding.base64.Encode(a.Data)
    var i = 0
    for i < len(enc) {
        var end = i + 76
        if end > len(enc) {
            end = len(enc)
        }
        b.Write(enc[i..end] + "\r\n")
        i = end
    }
    return b.String()
}

// ---- SMTP client --------------------------------------------------------------------

// Credentials for AUTH. Mechanism "" picks CRAM-MD5, then PLAIN, then
// LOGIN among those the server offers (PLAIN/LOGIN only over TLS unless
// AllowInsecureAuth).
struct Auth {
    Username  string
    Password  string
    Mechanism string
}

// PlainAuth / LoginAuth / CramMD5Auth make an Auth for one mechanism.
func PlainAuth(user string, password string) *Auth {
    return new Auth{Username: user, Password: password, Mechanism: "PLAIN"}
}

func LoginAuth(user string, password string) *Auth {
    return new Auth{Username: user, Password: password, Mechanism: "LOGIN"}
}

func CramMD5Auth(user string, password string) *Auth {
    return new Auth{Username: user, Password: password, Mechanism: "CRAM-MD5"}
}

// SMTP connection options.
struct Config {
    // "auto" (default: implicit TLS on port 465, else STARTTLS when
    // offered), "starttls" (required), "implicit", "none".
    TLS string = "auto"
    // TLS settings (ServerName defaults to the host).
    TLSConfig std.net.tls.Config
    // EHLO name (default "localhost").
    HelloName string = "localhost"
    // Per-operation timeout in ms (0: none).
    Timeout int = 60000
    // Allow PLAIN/LOGIN over a plain connection (tests, local relays).
    AllowInsecureAuth bool
}

// An SMTP error reply (code >= 400).
struct SMTPError {
    Code int
    Msg  string

    func Message() string {
        return "smtp: " + std.fmt.Format("{0}", this.Code) + " " + this.Msg
    }

    // 4xx: try again later.
    func Temporary() bool {
        return this.Code >= 400 && this.Code < 500
    }
}

interface stream {
    func Read(n int) Task<[]byte!>
    func Write(data []byte) Task<int!>
    func Close()
    func SetTimeout(ms int)
}

// A reply: code and its lines.
struct Reply {
    Code  int
    Lines []string

    func Text() string {
        return std.strings.Join(this.Lines, "\n")
    }
}

// An SMTP client session (Dial). Methods follow the protocol order:
// Hello (done by Dial), StartTLS, Authenticate, Mail, Rcpt, Data, Quit.
struct Client {
    s    stream
    raw  std.net.Conn
    buf  string
    host string
    cfg  Config
    ext  map[string]string
    tls  bool

    async func line() string! {
        for {
            var i = std.strings.Index(this.buf, "\n")
            if i >= 0 {
                var l = this.buf[..i]
                this.buf = this.buf[i + 1..]
                if std.strings.HasSuffix(l, "\r") {
                    l = l[..len(l) - 1]
                }
                return l
            }
            var chunk = await this.s.Read(4096)?
            if len(chunk) == 0 {
                return error("smtp: connection closed by the server")
            }
            this.buf = this.buf + std.strings.FromBytes(chunk)
        }
    }

    // Reads a (multi-line) reply.
    async func reply() Reply! {
        var r = Reply{}
        for {
            var l = await this.line()?
            if len(l) < 3 {
                return error("smtp: malformed reply \"" + l + "\"")
            }
            var code = std.strconv.Atoi(l[0..3])?("smtp: malformed reply")
            r.Code = code
            var rest = ""
            if len(l) > 4 {
                rest = l[4..]
            }
            r.Lines = append(r.Lines, rest)
            if len(l) == 3 || l[3..4] != "-" {
                return r
            }
        }
    }

    // Sends a command and reads the reply; codes outside expect fail.
    async func Cmd(line string, expect int) Reply! {
        if line != "" {
            _ = await this.s.Write(std.strings.Bytes(line + "\r\n"))?
        }
        var r = await this.reply()?
        if expect > 0 && r.Code / 100 != expect / 100 {
            return SMTPError{Code: r.Code, Msg: r.Text()}
        }
        return r
    }

    async func hello() void! {
        var r = try await this.Cmd("EHLO " + this.cfg.HelloName, 250) catch err {
            _ = await this.Cmd("HELO " + this.cfg.HelloName, 250)?
            this.ext = map[string]string{}
            return
        }
        this.ext = map[string]string{}
        for i, l in r.Lines {
            if i == 0 {
                continue
            }
            var sp = std.strings.Index(l, " ")
            if sp < 0 {
                this.ext[std.strings.ToUpper(l)] = ""
            } else {
                this.ext[std.strings.ToUpper(l[..sp])] = l[sp + 1..]
            }
        }
    }

    // Whether the server advertised an extension ("STARTTLS", "AUTH",
    // "SIZE", "8BITMIME", ...), and its parameters.
    func Extension(name string) (bool, string) {
        var k = std.strings.ToUpper(name)
        for key, v in this.ext {
            if key == k {
                return (true, v)
            }
        }
        return (false, "")
    }

    func IsTLS() bool {
        return this.tls
    }

    // Upgrades the connection with STARTTLS and says EHLO again.
    async func StartTLS() void! {
        _ = await this.Cmd("STARTTLS", 220)?
        var tc = this.cfg.TLSConfig
        if tc.ServerName == "" {
            tc.ServerName = this.host
        }
        this.buf = ""
        var t = await std.net.tls.Client(this.raw, tc)?
        await t.Handshake()?
        t.SetTimeout(this.cfg.Timeout)
        this.s = t
        this.tls = true
        await this.hello()?
    }

    async func Authenticate(a Auth) void! {
        var (ok, mechs) = this.Extension("AUTH")
        if !ok {
            return error("smtp: the server does not support AUTH")
        }
        var list = std.strings.Split(std.strings.ToUpper(mechs), " ")
        var mech = std.strings.ToUpper(a.Mechanism)
        if mech == "" {
            for m in ["CRAM-MD5", "PLAIN", "LOGIN"] {
                if mech == "" && contains(list, m) {
                    mech = m
                }
            }
            if mech == "" {
                return error("smtp: no supported AUTH mechanism among " + mechs)
            }
        }
        if (mech == "PLAIN" || mech == "LOGIN") && !this.tls && !this.cfg.AllowInsecureAuth {
            return error("smtp: refusing AUTH " + mech + " over an unencrypted connection")
        }
        if mech == "PLAIN" {
            var resp = std.encoding.base64.EncodeString("\u{0}" + a.Username + "\u{0}" + a.Password)
            _ = await this.Cmd("AUTH PLAIN " + resp, 235)?
        } else if mech == "LOGIN" {
            _ = await this.Cmd("AUTH LOGIN", 334)?
            _ = await this.Cmd(std.encoding.base64.EncodeString(a.Username), 334)?
            _ = await this.Cmd(std.encoding.base64.EncodeString(a.Password), 235)?
        } else if mech == "CRAM-MD5" {
            var r = await this.Cmd("AUTH CRAM-MD5", 334)?
            var challenge = std.encoding.base64.DecodeString(
                std.strings.Trim(r.Lines[0]),
            )?("smtp: CRAM-MD5 challenge")
            var digest = std.encoding.hex.Encode(
                HmacMD5(std.strings.Bytes(a.Password), std.strings.Bytes(challenge)),
            )
            _ = await this.Cmd(std.encoding.base64.EncodeString(a.Username + " " + digest), 235)?
        } else {
            return error("smtp: unsupported AUTH mechanism " + mech)
        }
    }

    async func Mail(from string) void! {
        _ = await this.Cmd("MAIL FROM:<" + from + ">", 250)?
    }

    async func Rcpt(to string) void! {
        _ = await this.Cmd("RCPT TO:<" + to + ">", 250)?
    }

    // Sends the message text (dot-stuffed).
    async func Data(text string) void! {
        _ = await this.Cmd("DATA", 354)?
        _ = await this.s.Write(std.strings.Bytes(std.net.textproto.DotEncode(text)))?
        _ = await this.Cmd("", 250)?
    }

    async func Reset() void! {
        _ = await this.Cmd("RSET", 250)?
    }

    async func Quit() void! {
        _ = try await this.Cmd("QUIT", 221) else Reply{}
        this.s.Close()
    }

    func Close() {
        this.s.Close()
    }
}

func contains(list []string, s string) bool {
    for x in list {
        if x == s {
            return true
        }
    }
    return false
}

// HMAC-MD5 (RFC 2104) for CRAM-MD5.
func HmacMD5(key []byte, msg []byte) []byte {
    var k = key
    if len(k) > 64 {
        k = std.crypto.hash.Md5(std.strings.FromBytes(k))
    }
    var ipad []byte = []
    var opad []byte = []
    for i in 0..64 {
        var b = 0
        if i < len(k) {
            b = int(k[i])
        }
        ipad = append(ipad, byte(b ^ 54))
        opad = append(opad, byte(b ^ 92))
    }
    var inner = std.crypto.hash.Md5(std.strings.FromBytes(ipad) + std.strings.FromBytes(msg))
    return std.crypto.hash.Md5(std.strings.FromBytes(opad) + std.strings.FromBytes(inner))
}

// Connects to addr ("host:port"), reads the greeting and says EHLO;
// TLS per cfg.TLS.
async func Dial(addr string, cfg Config) *Client! {
    var hp = std.net.SplitHostPort(addr)?
    var raw = await std.net.DialTimeout(addr, cfg.Timeout)?
    raw.SetTimeout(cfg.Timeout)
    var c = new Client{s: raw, raw: raw, host: hp.Host, cfg: cfg}
    var mode = cfg.TLS
    if mode == "" {
        mode = "auto"
    }
    if mode == "implicit" || (mode == "auto" && hp.Port == 465) {
        var tc = cfg.TLSConfig
        if tc.ServerName == "" {
            tc.ServerName = hp.Host
        }
        var t = try await std.net.tls.Client(raw, tc) catch err {
            raw.Close()
            return err
        }
        try await t.Handshake() catch err {
            t.Close()
            return err
        }
        t.SetTimeout(cfg.Timeout)
        c.s = t
        c.tls = true
    }
    try {
        _ = await c.Cmd("", 220)?
        await c.hello()?
        if !c.tls && mode != "none" {
            var (has, _) = c.Extension("STARTTLS")
            if has {
                await c.StartTLS()?
            } else if mode == "starttls" {
                return error("smtp: the server does not offer STARTTLS")
            }
        }
    } catch err {
        c.Close()
        return err
    }
    return c
}

// Sends msg through the server at addr with default options (see
// SendWith).
async func Send(addr string, auth *Auth, msg Message) void! {
    var cfg Config
    await SendWith(addr, cfg, auth, msg)?
}

// Connects, upgrades to TLS, authenticates (when auth is not nil) and
// sends msg to all its recipients.
async func SendWith(addr string, cfg Config, auth *Auth, msg Message) void! {
    var text = msg.Build()?
    var c = await Dial(addr, cfg)?
    try {
        if auth != nil {
            await c.Authenticate(*auth)?
        }
        await c.Mail(msg.From.Address)?
        for r in msg.Recipients() {
            await c.Rcpt(r)?
        }
        await c.Data(text)?
    } catch err {
        c.Close()
        return err
    }
    await c.Quit()?
}
