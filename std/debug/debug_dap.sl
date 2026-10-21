// std.debug, part 3: Debug Adapter Protocol framing (Content-Length
// headers over the agent's blocking socket) and JSON output helpers.
namespace std.debug

// A DAP request.
struct request {
    seq     int
    command string
    args    std.encoding.json.Value
}

func parseRequest(body string) request! {
    var v = std.encoding.json.ParseValue(body)?
    var seq = try v.Get("seq").Int() else 0
    var cmd = v.Get("command").String()?
    return request{seq: seq, command: cmd, args: v.Get("arguments")}
}

// An integer argument of a request (0 if absent).
func argInt(req request, name string) int {
    return try req.args.Get(name).Int() else 0
}

// Reads Content-Length framed messages from a socket.
struct reader {
    fd  int
    buf string

    func next() string! {
        for {
            var end = std.strings.Index(this.buf, "\r\n\r\n")
            if end >= 0 {
                var n = -1
                for line in std.strings.Split(this.buf[..end], "\r\n") {
                    var colon = std.strings.Index(line, ":")
                    if colon > 0 &&
                        std.strings.EqualFold(std.strings.Trim(line[..colon]), "Content-Length") {
                        n = std.strconv.Atoi(std.strings.Trim(line[colon + 1..]))?
                    }
                }
                if n < 0 {
                    return error("debug: message without Content-Length")
                }
                var start = end + 4
                if len(this.buf) >= start + n {
                    var body = this.buf[start..start + n]
                    this.buf = this.buf[start + n..]
                    return body
                }
            }
            var chunk = rtRecv(this.fd, 65536)
            if len(chunk) == 0 {
                return error("debug: connection closed")
            }
            this.buf = this.buf + std.strings.FromBytes(chunk)
        }
    }
}

// A JSON string literal.
func q(s string) string {
    return std.encoding.json.Quote(s)
}

// A JSON object from alternating keys and already encoded values.
func obj(kv []string) string {
    var parts []string = []
    var i = 0
    for i + 1 < len(kv) {
        parts = append(parts, q(kv[i]) + ":" + kv[i + 1])
        i += 2
    }
    return "{" + std.strings.Join(parts, ",") + "}"
}
