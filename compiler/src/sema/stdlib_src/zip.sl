namespace std.archive.zip

// ZIP archives (PKWARE APPNOTE 6.3): reading and writing in memory,
// methods Store (0) and Deflate (8, through std.compress), Zip64 records
// for large entries/archives (read always; written when needed or with
// Writer.Zip64), the extended timestamp extra field for exact UTC
// modification times, UTF-8 names. Encryption and other methods are not
// supported. ExtractTo / FromDir move archives to and from directories.

// The compression methods are the constants zip.Store (0) and
// zip.Deflate (8).

// One entry of an archive.
struct File {
    Name string
    Comment string
    Method int
    // Unix seconds, UTC.
    Modified int
    // Unix permission bits (0 when the archive does not say).
    Mode int
    CRC32 int
    CompressedSize int
    Size int
    offset int
    flags int
    data []byte

    func IsDir() bool {
        return std.strings.HasSuffix(this.Name, "/")
    }

    // The uncompressed content (checked against CRC32).
    func Bytes() []byte! {
        return readEntry(this.data, *this)?
    }

    func Text() string! {
        return std.strings.FromBytes(this.Bytes()?)
    }
}

// An opened archive.
struct Reader {
    Files []File
    Comment string

    // The entry named name, or nil.
    func File(name string) *File {
        for f in this.Files {
            if f.Name == name {
                var copy = f
                return &copy
            }
        }
        return nil
    }

    // The content of the entry named name.
    func Read(name string) []byte! {
        var f = this.File(name)
        if f == nil {
            return error("zip: no file " + name + " in the archive")
        }
        return f.Bytes()?
    }

    // The entry names in archive order.
    func Names() []string {
        var out []string = []
        for f in this.Files {
            out = append(out, f.Name)
        }
        return out
    }
}

func u16(b []byte, i int) int {
    return int(b[i]) | (int(b[i + 1]) << 8)
}

func u32(b []byte, i int) int {
    return int(b[i]) | (int(b[i + 1]) << 8) | (int(b[i + 2]) << 16) | (int(b[i + 3]) << 24)
}

func u64(b []byte, i int) int {
    return u32(b, i) | (u32(b, i + 4) << 32)
}

// Parses an archive held in memory.
func Open(data []byte) Reader! {
    var n = len(data)
    // end of central directory: the last signature within 64 KB + 22 bytes
    var eocd = -1
    var i = n - 22
    var stop = std.math.IMax(0, n - 65557)
    for i >= stop {
        if int(data[i]) == 80 && int(data[i + 1]) == 75 && int(data[i + 2]) == 5 && int(data[i + 3]) == 6 {
            eocd = i
            break
        }
        i -= 1
    }
    if eocd < 0 {
        return error("zip: not a valid zip file")
    }
    var count = u16(data, eocd + 10)
    var cdSize = u32(data, eocd + 12)
    var cdOff = u32(data, eocd + 16)
    var clen = u16(data, eocd + 20)
    var comment = ""
    if eocd + 22 + clen <= n {
        comment = std.strings.FromBytes(std.compress.rtCopy(data, eocd + 22, eocd + 22 + clen))
    }
    // zip64 end of central directory locator
    if eocd >= 20 && u32(data, eocd - 20) == 117853008 {
        var z64 = u64(data, eocd - 12)
        if z64 < 0 || z64 + 56 > n || u32(data, z64) != 101075792 {
            return error("zip: invalid zip64 end of central directory")
        }
        count = u64(data, z64 + 32)
        cdSize = u64(data, z64 + 40)
        cdOff = u64(data, z64 + 48)
    }
    if cdOff < 0 || cdSize < 0 || cdOff + cdSize > n {
        return error("zip: central directory out of range")
    }
    var files []File = []
    var p = cdOff
    for _ in 0..count {
        if p + 46 > n || u32(data, p) != 33639248 {
            return error("zip: invalid central directory entry")
        }
        var f = File{data: data}
        f.flags = u16(data, p + 8)
        f.Method = u16(data, p + 10)
        var dtime = u16(data, p + 12)
        var ddate = u16(data, p + 14)
        f.Modified = fromDos(ddate, dtime)
        f.CRC32 = u32(data, p + 16)
        f.CompressedSize = u32(data, p + 20)
        f.Size = u32(data, p + 24)
        var nlen = u16(data, p + 28)
        var elen = u16(data, p + 30)
        var flen = u16(data, p + 32)
        var made = u16(data, p + 4) >> 8
        var ext = u32(data, p + 38)
        f.offset = u32(data, p + 42)
        if p + 46 + nlen + elen + flen > n {
            return error("zip: invalid central directory entry")
        }
        f.Name = std.strings.FromBytes(std.compress.rtCopy(data, p + 46, p + 46 + nlen))
        f.Comment = std.strings.FromBytes(std.compress.rtCopy(data, p + 46 + nlen + elen, p + 46 + nlen + elen + flen))
        if made == 3 {
            f.Mode = (ext >> 16) & 4095
        }
        // extra fields: zip64 sizes/offset, extended timestamp
        var e = p + 46 + nlen
        var eend = e + elen
        for e + 4 <= eend {
            var tag = u16(data, e)
            var size = u16(data, e + 2)
            var body = e + 4
            if body + size > eend {
                break
            }
            if tag == 1 {
                var q = body
                if f.Size == 4294967295 && q + 8 <= body + size {
                    f.Size = u64(data, q)
                    q += 8
                }
                if f.CompressedSize == 4294967295 && q + 8 <= body + size {
                    f.CompressedSize = u64(data, q)
                    q += 8
                }
                if f.offset == 4294967295 && q + 8 <= body + size {
                    f.offset = u64(data, q)
                }
            } else if tag == 21589 && size >= 5 && (int(data[body]) & 1) != 0 {
                var t = u32(data, body + 1)
                if t >= 2147483648 {
                    t -= 4294967296
                }
                f.Modified = t
            }
            e = body + size
        }
        files = append(files, f)
        p += 46 + nlen + elen + flen
    }
    return Reader{Files: files, Comment: comment}
}

func readEntry(data []byte, f File) []byte! {
    if (f.flags & 1) != 0 {
        return error("zip: " + f.Name + ": encrypted entries are not supported")
    }
    var p = f.offset
    if p < 0 || p + 30 > len(data) || u32(data, p) != 67324752 {
        return error("zip: " + f.Name + ": invalid local file header")
    }
    var start = p + 30 + u16(data, p + 26) + u16(data, p + 28)
    if start + f.CompressedSize > len(data) || f.CompressedSize < 0 {
        return error("zip: " + f.Name + ": data out of range")
    }
    var raw = std.compress.rtCopy(data, start, start + f.CompressedSize)
    var out []byte = []
    if f.Method == 0 {
        out = raw
    } else if f.Method == 8 {
        out = try std.compress.deflate.Decompress(raw) catch err {
            return error("zip: " + f.Name + ": " + err.Message())
        }
    } else {
        return error("zip: " + f.Name + ": unsupported compression method " + std.fmt.Format("{0}", f.Method))
    }
    if len(out) != f.Size {
        return error("zip: " + f.Name + ": size mismatch")
    }
    if std.compress.Crc32(0, out) != f.CRC32 {
        return error("zip: " + f.Name + ": checksum error")
    }
    return out
}

// -- writing ----------------------------------------------------------------

// What to record for an entry added with Writer.AddHeader.
struct Header {
    Name string
    Comment string
    Method int = 8
    Modified int
    Mode int = 420
}

// Builds an archive in memory: Add entries, then Close() returns the bytes.
struct Writer {
    // Level for deflated entries (-1: default).
    Level int = -1
    // Write Zip64 records even for small archives.
    Zip64 bool
    Comment string
    buf []byte
    entries []File
    names map[string]bool
    closed bool

    // Adds a file with the defaults (deflate, mode 0644, time 0).
    func Add(name string, data []byte) void! {
        this.AddHeader(Header{Name: name}, data)?
    }

    func AddString(name string, text string) void! {
        this.AddHeader(Header{Name: name}, std.strings.Bytes(text))?
    }

    // Adds a directory entry ("name/").
    func AddDir(name string, modified int) void! {
        var n = name
        if !std.strings.HasSuffix(n, "/") {
            n = n + "/"
        }
        var none []byte = []
        this.AddHeader(Header{Name: n, Method: 0, Modified: modified, Mode: 493}, none)?
    }

    func AddHeader(h Header, data []byte) void! {
        if this.closed {
            return error("zip: Add after Close")
        }
        if h.Name == "" {
            return error("zip: empty file name")
        }
        if this.names == nil {
            this.names = map[string]bool{}
        }
        if this.names[h.Name] {
            return error("zip: duplicate file name " + h.Name)
        }
        if h.Method != 0 && h.Method != 8 {
            return error("zip: unsupported compression method " + std.fmt.Format("{0}", h.Method))
        }
        this.names[h.Name] = true
        var comp = data
        if h.Method == 8 {
            comp = std.compress.deflate.Compress(data, this.Level)
        }
        var f = File{Name: h.Name, Comment: h.Comment, Method: h.Method, Modified: h.Modified, Mode: h.Mode, CRC32: std.compress.Crc32(0, data), CompressedSize: len(comp), Size: len(data), offset: len(this.buf)}
        var big = this.Zip64 || len(comp) >= 4294967295 || len(data) >= 4294967295 || f.offset >= 4294967295
        // local file header
        var b = this.buf
        b = put32(b, 67324752)
        if big {
            b = put16(b, 45)
        } else {
            b = put16(b, 20)
        }
        b = put16(b, 2048)
        b = put16(b, h.Method)
        var dos = toDos(h.Modified)
        b = put16(b, dos[1])
        b = put16(b, dos[0])
        b = put32(b, f.CRC32)
        if big {
            b = put32(b, 4294967295)
            b = put32(b, 4294967295)
        } else {
            b = put32(b, len(comp))
            b = put32(b, len(data))
        }
        var nameBytes = std.strings.Bytes(h.Name)
        b = put16(b, len(nameBytes))
        var extra = timeExtra(h.Modified)
        if big {
            var z []byte = []
            z = put16(z, 1)
            z = put16(z, 16)
            z = put64(z, len(data))
            z = put64(z, len(comp))
            extra = std.compress.rtConcat(z, extra)
        }
        b = put16(b, len(extra))
        b = std.compress.rtConcat(b, nameBytes)
        b = std.compress.rtConcat(b, extra)
        b = std.compress.rtConcat(b, comp)
        this.buf = b
        this.entries = append(this.entries, f)
    }

    // Finishes the archive and returns its bytes.
    func Close() []byte {
        if this.closed {
            return this.buf
        }
        this.closed = true
        var b = this.buf
        var cdOff = len(b)
        var any64 = this.Zip64 || len(this.entries) >= 65535
        for f in this.entries {
            var big = this.Zip64 || f.Size >= 4294967295 || f.CompressedSize >= 4294967295 || f.offset >= 4294967295
            if big {
                any64 = true
            }
            b = put32(b, 33639248)
            b = put16(b, 798)
            if big {
                b = put16(b, 45)
            } else {
                b = put16(b, 20)
            }
            b = put16(b, 2048)
            b = put16(b, f.Method)
            var dos = toDos(f.Modified)
            b = put16(b, dos[1])
            b = put16(b, dos[0])
            b = put32(b, f.CRC32)
            var extra = timeExtra(f.Modified)
            if big {
                b = put32(b, 4294967295)
                b = put32(b, 4294967295)
                var z []byte = []
                z = put16(z, 1)
                z = put16(z, 24)
                z = put64(z, f.Size)
                z = put64(z, f.CompressedSize)
                z = put64(z, f.offset)
                extra = std.compress.rtConcat(z, extra)
            } else {
                b = put32(b, f.CompressedSize)
                b = put32(b, f.Size)
            }
            var nameBytes = std.strings.Bytes(f.Name)
            var commentBytes = std.strings.Bytes(f.Comment)
            b = put16(b, len(nameBytes))
            b = put16(b, len(extra))
            b = put16(b, len(commentBytes))
            b = put16(b, 0)
            b = put16(b, 0)
            var mode = f.Mode
            if f.IsDir() {
                mode = mode | 16384
            } else {
                mode = mode | 32768
            }
            var ext = mode << 16
            if f.IsDir() {
                ext = ext | 16
            }
            b = put32(b, ext)
            if big {
                b = put32(b, 4294967295)
            } else {
                b = put32(b, f.offset)
            }
            b = std.compress.rtConcat(b, nameBytes)
            b = std.compress.rtConcat(b, extra)
            b = std.compress.rtConcat(b, commentBytes)
        }
        var cdSize = len(b) - cdOff
        if cdOff >= 4294967295 || cdSize >= 4294967295 {
            any64 = true
        }
        if any64 {
            var z64 = len(b)
            b = put32(b, 101075792)
            b = put64(b, 44)
            b = put16(b, 798)
            b = put16(b, 45)
            b = put32(b, 0)
            b = put32(b, 0)
            b = put64(b, len(this.entries))
            b = put64(b, len(this.entries))
            b = put64(b, cdSize)
            b = put64(b, cdOff)
            b = put32(b, 117853008)
            b = put32(b, 0)
            b = put64(b, z64)
            b = put32(b, 1)
        }
        b = put32(b, 101010256)
        b = put16(b, 0)
        b = put16(b, 0)
        var n16 = std.math.IMin(len(this.entries), 65535)
        if any64 {
            n16 = 65535
        }
        b = put16(b, n16)
        b = put16(b, n16)
        if any64 {
            b = put32(b, 4294967295)
            b = put32(b, 4294967295)
        } else {
            b = put32(b, cdSize)
            b = put32(b, cdOff)
        }
        var cb = std.strings.Bytes(this.Comment)
        b = put16(b, len(cb))
        b = std.compress.rtConcat(b, cb)
        this.buf = b
        return b
    }
}

func NewWriter() *Writer {
    return &Writer{Level: -1}
}

func put16(b []byte, v int) []byte {
    var out = append(b, byte(v & 255))
    return append(out, byte((v >> 8) & 255))
}

func put32(b []byte, v int) []byte {
    return put16(put16(b, v & 65535), (v >> 16) & 65535)
}

func put64(b []byte, v int) []byte {
    return put32(put32(b, v & 4294967295), (v >> 32) & 4294967295)
}

// The extended timestamp extra field (0x5455) with the modification time.
func timeExtra(t int) []byte {
    var b []byte = []
    b = put16(b, 21589)
    b = put16(b, 5)
    b = append(b, byte(1))
    return put32(b, t & 4294967295)
}

// -- MS-DOS date and time (treated as UTC) ----------------------------------

// Days since 1970-01-01 of a civil date (proleptic Gregorian).
func daysFromCivil(y0 int, m int, d int) int {
    var y = y0
    if m <= 2 {
        y -= 1
    }
    var era = y / 400
    if y < 0 && y % 400 != 0 {
        era -= 1
    }
    var yoe = y - era * 400
    var mp = (m + 9) % 12
    var doy = (153 * mp + 2) / 5 + d - 1
    var doe = yoe * 365 + yoe / 4 - yoe / 100 + doy
    return era * 146097 + doe - 719468
}

// [year, month, day] of a day count since 1970-01-01.
func civilFromDays(z0 int) []int {
    var z = z0 + 719468
    var era = z / 146097
    if z < 0 && z % 146097 != 0 {
        era -= 1
    }
    var doe = z - era * 146097
    var yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365
    var y = yoe + era * 400
    var doy = doe - (365 * yoe + yoe / 4 - yoe / 100)
    var mp = (5 * doy + 2) / 153
    var d = doy - (153 * mp + 2) / 5 + 1
    var m = mp + 3
    if m > 12 {
        m -= 12
    }
    if m <= 2 {
        y += 1
    }
    return [y, m, d]
}

func fromDos(date int, time int) int {
    if date == 0 {
        return 0
    }
    var y = (date >> 9) + 1980
    var m = (date >> 5) & 15
    var d = date & 31
    if m < 1 || m > 12 || d < 1 {
        return 0
    }
    var secs = ((time >> 11) & 31) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2
    return daysFromCivil(y, m, d) * 86400 + secs
}

// [date, time] in MS-DOS format (1980-01-01 for earlier times).
func toDos(t int) []int {
    if t < 315532800 {
        return [33, 0]
    }
    var days = t / 86400
    var secs = t % 86400
    var c = civilFromDays(days)
    if c[0] > 2107 {
        return [65439, 48991]
    }
    var date = ((c[0] - 1980) << 9) | (c[1] << 5) | c[2]
    var time = ((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2)
    return [date, time]
}

// -- directories ----------------------------------------------------------------

// Reports whether an entry name is safe to extract (relative, no "..").
func safeName(name string) bool {
    if name == "" || std.strings.HasPrefix(name, "/") || std.strings.HasPrefix(name, "\\") || std.strings.Contains(name, ":") {
        return false
    }
    for part in std.strings.Split(std.strings.Replace(name, "\\", "/"), "/") {
        if part == ".." {
            return false
        }
    }
    return true
}

// Extracts every entry of the archive under dir (created if needed).
// Names that would escape dir are an error.
func ExtractTo(data []byte, dir string) void! {
    var r = Open(data)?
    std.fs.MkdirAll(dir)?
    for f in r.Files {
        if !safeName(f.Name) {
            return error("zip: unsafe file name " + f.Name)
        }
        var path = std.fs.Join(dir, f.Name)
        if f.IsDir() {
            std.fs.MkdirAll(path)?
            continue
        }
        std.fs.MkdirAll(std.fs.Dir(path))?
        var content = f.Bytes()?
        std.io.WriteFile(path, std.strings.FromBytes(content))?
    }
}

// Archives the files under dir (recursively; names relative to dir, '/'
// separated, sorted; directories get entries too).
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
            var text = std.io.ReadFile(full)?
            w.AddHeader(Header{Name: r, Modified: mt}, std.strings.Bytes(text))?
        }
    }
}
