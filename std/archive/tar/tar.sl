// tar archives (POSIX.1-2001 ustar/pax, GNU long names and base-256
// numbers on read): reading and writing in memory. Long or non-ASCII
// names, big sizes and big ids are written as pax extended headers.
// ExtractTo / FromDir move archives to and from directories (regular
// files and directories; links are listed but not created).
namespace std.archive.tar

// Entry types (Header.Type) are the constants tar.TypeReg ('0'),
// TypeLink ('1'), TypeSymlink ('2'), TypeDir ('5').

// One entry header.
struct Header {
    Name string
    // '0' regular, '5' directory, '2' symlink, '1' hard link (bytes 48..).
    Type int = 48
    Mode int = 420
    Uid  int
    Gid  int
    Size int
    // Unix seconds.
    ModTime  int
    Linkname string
    Uname    string
    Gname    string

    func IsDir() bool {
        return this.Type == 53
    }

    func IsRegular() bool {
        return this.Type == 48 || this.Type == 0 || this.Type == 55
    }
}

// An entry: its header and content.
struct Entry {
    Header Header
    Data   []byte
}

// Reads every entry of an archive.
func Read(data []byte) []Entry! {
    var r = NewReader(data)
    var out []Entry = []
    for {
        var h = r.Next()?
        if h == nil {
            break
        }
        out = append(out, Entry{Header: *h, Data: r.Data()})
    }
    return out
}

// Sequential access to the entries of an archive in memory.
struct Reader {
    data   []byte
    pos    int
    cur    []byte
    global map[string]string
    done   bool

    // The next entry's header, or nil at the end of the archive.
    func Next() *Header! {
        var pax = map[string]string{}
        var longName = ""
        var longLink = ""
        for {
            if this.done || this.pos + 512 > len(this.data) {
                this.done = true
                return nil
            }
            var b = this.data
            var p = this.pos
            if allZero(b, p, p + 512) {
                this.done = true
                return nil
            }
            if !checksumOK(b, p) {
                return error("tar: invalid header checksum")
            }
            var h = Header{}
            h.Name = cstr(b, p, 100)
            h.Mode = num(b, p + 100, 8)?
            h.Uid = num(b, p + 108, 8)?
            h.Gid = num(b, p + 116, 8)?
            h.Size = num(b, p + 124, 12)?
            h.ModTime = num(b, p + 136, 12)?
            h.Type = int(b[p + 156])
            h.Linkname = cstr(b, p + 157, 100)
            var magic = cstr(b, p + 257, 6)
            if magic == "ustar" {
                h.Uname = cstr(b, p + 265, 32)
                h.Gname = cstr(b, p + 297, 32)
                var prefix = cstr(b, p + 345, 155)
                if prefix != "" && cstr(b, p + 257, 8) != "ustar  " {
                    h.Name = prefix + "/" + h.Name
                }
            }
            if h.Size < 0 {
                return error("tar: invalid size")
            }
            var start = p + 512
            var end = start + h.Size
            if end > len(b) {
                return error("tar: unexpected end of archive")
            }
            this.pos = start + (h.Size + 511) / 512 * 512
            var body = std.compress.rtCopy(b, start, end)
            if h.Type == 120 || h.Type == 103 {
                // pax extended header (x: next entry, g: global)
                var recs = parsePax(body)?
                for k, v in recs {
                    if h.Type == 103 {
                        if this.global == nil {
                            this.global = map[string]string{}
                        }
                        this.global[k] = v
                    } else {
                        pax[k] = v
                    }
                }
                continue
            }
            if h.Type == 76 {
                longName = trimNul(std.strings.FromBytes(body))
                continue
            }
            if h.Type == 75 {
                longLink = trimNul(std.strings.FromBytes(body))
                continue
            }
            if this.global != nil {
                applyPax(&h, this.global)?
            }
            applyPax(&h, pax)?
            if longName != "" {
                h.Name = longName
            }
            if longLink != "" {
                h.Linkname = longLink
            }
            if pax["size"] != "" {
                // the content size given by pax
                end = start + h.Size
                if end > len(b) {
                    return error("tar: unexpected end of archive")
                }
                body = std.compress.rtCopy(b, start, end)
                this.pos = start + (h.Size + 511) / 512 * 512
            }
            this.cur = body
            return &h
        }
    }

    // The content of the entry returned by the last Next.
    func Data() []byte {
        return this.cur
    }
}

func NewReader(data []byte) *Reader {
    return &Reader{data: data}
}

func allZero(b []byte, from int, to int) bool {
    for i in from..to {
        if int(b[i]) != 0 {
            return false
        }
    }
    return true
}

func checksumOK(b []byte, p int) bool {
    var want = try parseOctal(b, p + 148, 8) else -1
    var sum = 0
    var signed = 0
    for i in 0..512 {
        var c = int(b[p + i])
        if i >= 148 && i < 156 {
            c = 32
        }
        sum += c
        if c >= 128 {
            signed += c - 256
        } else {
            signed += c
        }
    }
    return want == sum || want == signed
}

// A NUL-terminated string field.
func cstr(b []byte, p int, n int) string {
    var end = p
    for end < p + n && int(b[end]) != 0 {
        end += 1
    }
    return std.strings.FromBytes(std.compress.rtCopy(b, p, end))
}

func trimNul(s string) string {
    for i in 0..len(s) {
        if int(s[i]) == 0 {
            return s[..i]
        }
    }
    return s
}

// A numeric field: octal (space/NUL padded) or GNU base-256.
func num(b []byte, p int, n int) int! {
    if (int(b[p]) & 128) != 0 {
        var v = int(b[p]) & 127
        if (v & 64) != 0 {
            v = v - 128
        }
        for i in 1..n {
            v = (v << 8) | int(b[p + i])
        }
        return v
    }
    return parseOctal(b, p, n)?
}

func parseOctal(b []byte, p int, n int) int! {
    var v = 0
    var seen = false
    for i in p..(p + n) {
        var c = int(b[i])
        if c == 0 || (c == 32 && seen) {
            break
        }
        if c == 32 {
            continue
        }
        if c < 48 || c > 55 {
            return error("tar: invalid number in header")
        }
        v = v * 8 + c - 48
        seen = true
    }
    return v
}

// Records "len key=value\n" of a pax header.
func parsePax(body []byte) map[string]string! {
    var out = map[string]string{}
    var s = std.strings.FromBytes(body)
    var i = 0
    for i < len(s) {
        var sp = std.strings.Index(s[i..], " ")
        if sp <= 0 {
            return error("tar: invalid pax record")
        }
        var n = try std.strings.ToInt(s[i..i + sp]) else -1
        if n <= sp || i + n > len(s) || int(s[i + n - 1]) != 10 {
            return error("tar: invalid pax record")
        }
        var rec = s[i + sp + 1..i + n - 1]
        var eq = std.strings.Index(rec, "=")
        if eq < 0 {
            return error("tar: invalid pax record")
        }
        out[rec[..eq]] = rec[eq + 1..]
        i += n
    }
    return out
}

func applyPax(h *Header, recs map[string]string) void! {
    for k, v in recs {
        if k == "path" {
            h.Name = v
        } else if k == "linkpath" {
            h.Linkname = v
        } else if k == "uname" {
            h.Uname = v
        } else if k == "gname" {
            h.Gname = v
        } else if k == "size" {
            h.Size = try std.strings.ToInt(v) catch err {
                return error("tar: invalid pax size " + v)
            }
        } else if k == "uid" {
            h.Uid = try std.strings.ToInt(v) else h.Uid
        } else if k == "gid" {
            h.Gid = try std.strings.ToInt(v) else h.Gid
        } else if k == "mtime" {
            var dot = std.strings.Index(v, ".")
            var whole = v
            if dot >= 0 {
                whole = v[..dot]
            }
            h.ModTime = try std.strings.ToInt(whole) else h.ModTime
        }
    }
}

// -- writing ----------------------------------------------------------------

// Builds an archive in memory: Add entries, then Close() returns the bytes.
struct Writer {
    buf    []byte
    closed bool

    // Adds a regular file (mode 0644, time 0).
    func Add(name string, data []byte) void! {
        this.AddHeader(Header{Name: name, Size: len(data)}, data)?
    }

    func AddString(name string, text string) void! {
        this.Add(name, std.strings.Bytes(text))?
    }

    // Adds a directory entry.
    func AddDir(name string, modTime int) void! {
        var n = name
        if !std.strings.HasSuffix(n, "/") {
            n = n + "/"
        }
        var none []byte = []
        this.AddHeader(Header{Name: n, Type: 53, Mode: 493, ModTime: modTime}, none)?
    }

    // Adds an entry; h.Size is set from data.
    func AddHeader(h0 Header, data []byte) void! {
        if this.closed {
            return error("tar: Add after Close")
        }
        var h = h0
        h.Size = len(data)
        if h.Name == "" {
            return error("tar: empty file name")
        }
        // what does not fit ustar goes into a pax header
        var pax []string = []
        var name = h.Name
        var prefix = ""
        if !ascii(name) || len(name) > 100 {
            var split = splitName(name)
            if ascii(name) && split >= 0 {
                prefix = name[..split]
                name = name[split + 1..]
            } else {
                pax = append(pax, record("path", h.Name))
                name = asciiPrefix(name, 100)
            }
        }
        var link = h.Linkname
        if !ascii(link) || len(link) > 100 {
            pax = append(pax, record("linkpath", link))
            link = asciiPrefix(link, 100)
        }
        if h.Size > 8589934591 {
            pax = append(pax, record("size", std.fmt.Format("{0}", h.Size)))
        }
        if h.Uid > 2097151 {
            pax = append(pax, record("uid", std.fmt.Format("{0}", h.Uid)))
        }
        if h.Gid > 2097151 {
            pax = append(pax, record("gid", std.fmt.Format("{0}", h.Gid)))
        }
        if h.ModTime < 0 || h.ModTime > 8589934591 {
            pax = append(pax, record("mtime", std.fmt.Format("{0}", h.ModTime)))
        }
        if !ascii(h.Uname) || len(h.Uname) > 32 {
            pax = append(pax, record("uname", h.Uname))
        }
        if !ascii(h.Gname) || len(h.Gname) > 32 {
            pax = append(pax, record("gname", h.Gname))
        }
        if len(pax) > 0 {
            var body = std.strings.Bytes(std.strings.Join(pax, ""))
            var ph = Header{
                Name: asciiPrefix("PaxHeaders/" + asciiPrefix(std.fs.Base(h.Name), 80), 100),
                Type: 120,
                Mode: 420,
                Size: len(body),
            }
            this.block(ph, ph.Name, "", "", body)
        }
        this.block(h, name, prefix, link, data)
    }

    func block(h Header, name string, prefix string, link string, data []byte) {
        var b = std.compress.rtZeros(512)
        field(b, 0, 100, name)
        octal(b, 100, 8, h.Mode)
        octal(b, 108, 8, clampNum(h.Uid, 2097151))
        octal(b, 116, 8, clampNum(h.Gid, 2097151))
        octal(b, 124, 12, clampNum(h.Size, 8589934591))
        octal(b, 136, 12, clampNum(h.ModTime, 8589934591))
        b[156] = byte(h.Type)
        field(b, 157, 100, link)
        field(b, 257, 6, "ustar")
        b[263] = byte(48)
        b[264] = byte(48)
        field(b, 265, 32, asciiPrefix(h.Uname, 32))
        field(b, 297, 32, asciiPrefix(h.Gname, 32))
        field(b, 345, 155, prefix)
        for i in 148..156 {
            b[i] = byte(32)
        }
        var sum = 0
        for x in b {
            sum += int(x)
        }
        octal(b, 148, 7, sum)
        b[155] = byte(32)
        this.buf = std.compress.rtConcat(this.buf, b)
        this.buf = std.compress.rtConcat(this.buf, data)
        var pad = (512 - len(data) % 512) % 512
        if pad > 0 {
            this.buf = std.compress.rtConcat(this.buf, std.compress.rtZeros(pad))
        }
    }

    // Finishes the archive (two zero blocks) and returns its bytes.
    func Close() []byte {
        if !this.closed {
            this.closed = true
            this.buf = std.compress.rtConcat(this.buf, std.compress.rtZeros(1024))
        }
        return this.buf
    }
}

func NewWriter() *Writer {
    return &Writer{}
}

func clampNum(v int, max int) int {
    if v < 0 {
        return 0
    }
    if v > max {
        return max
    }
    return v
}

func field(b []byte, p int, n int, s string) {
    var k = 0
    for i in 0..len(s) {
        if k >= n {
            break
        }
        b[p + k] = s[i]
        k += 1
    }
}

// n-1 octal digits and a NUL.
func octal(b []byte, p int, n int, v int) {
    var x = v
    var i = p + n - 2
    for i >= p {
        b[i] = byte(48 + (x & 7))
        x = x >> 3
        i -= 1
    }
    b[p + n - 1] = byte(0)
}

func ascii(s string) bool {
    for i in 0..len(s) {
        if int(s[i]) >= 128 || int(s[i]) == 0 {
            return false
        }
    }
    return true
}

func asciiPrefix(s string, n int) string {
    var b []byte = []
    for i in 0..len(s) {
        if len(b) >= n {
            break
        }
        var c = int(s[i])
        if c > 0 && c < 128 {
            b = append(b, s[i])
        } else {
            b = append(b, byte(95))
        }
    }
    return std.strings.FromBytes(b)
}

// A '/' that splits name into a ustar prefix (<= 155) and name (<= 100), or -1.
func splitName(name string) int {
    var i = len(name) - 1
    for i > 0 {
        if int(name[i]) == 47 && i <= 155 && len(name) - i - 1 <= 100 && len(name) - i - 1 > 0 {
            return i
        }
        i -= 1
    }
    return -1
}

// A pax record "len key=value\n" (len counts itself).
func record(k string, v string) string {
    var body = " " + k + "=" + v + "\n"
    var n = len(body) + 1
    var s = std.fmt.Format("{0}", n)
    for len(s) + len(body) != n {
        n = len(s) + len(body)
        s = std.fmt.Format("{0}", n)
    }
    return s + body
}

// -- directories ----------------------------------------------------------------

func safeName(name string) bool {
    if name == "" || std.strings.HasPrefix(name, "/") {
        return false
    }
    for part in std.strings.Split(name, "/") {
        if part == ".." {
            return false
        }
    }
    return true
}

// Extracts the regular files and directories of the archive under dir;
// names that would escape dir are an error, other entry types are skipped.
func ExtractTo(data []byte, dir string) void! {
    std.fs.MkdirAll(dir)?
    for e in Read(data)? {
        var name = e.Header.Name
        if !safeName(name) {
            return error("tar: unsafe file name " + name)
        }
        var path = std.fs.Join(dir, name)
        if e.Header.IsDir() {
            std.fs.MkdirAll(path)?
        } else if e.Header.IsRegular() {
            std.fs.MkdirAll(std.fs.Dir(path))?
            std.io.WriteFile(path, std.strings.FromBytes(e.Data))?
        }
    }
}

// Archives the files under dir (recursively, names relative to dir).
func FromDir(dir string) []byte! {
    var w = NewWriter()
    addDir(w, dir, "")?
    return w.Close()
}

func addDir(w *Writer, root string, rel string) void! {
    var path = root
    if rel != "" {
        path = std.fs.Join(root, rel)
    }
    for name in std.fs.ReadDir(path)? {
        var r = name
        if rel != "" {
            r = rel + "/" + name
        }
        var full = std.fs.Join(root, r)
        var mt = std.fs.ModTime(full)? / 1000
        if std.fs.IsDir(full) {
            w.AddDir(r, mt)?
            addDir(w, root, r)?
        } else {
            var data = std.strings.Bytes(std.io.ReadFile(full)?)
            w.AddHeader(Header{Name: r, ModTime: mt}, data)?
        }
    }
}
