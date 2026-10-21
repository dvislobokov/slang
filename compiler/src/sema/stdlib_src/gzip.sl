namespace std.compress.gzip

// gzip files (RFC 1952): members of header + raw DEFLATE + CRC-32 and
// size. Decompress and Reader read concatenated members as one stream,
// like gzip -d.

// The optional header fields of a member. ModTime is Unix seconds (0:
// unset); OS is the RFC 1952 operating system byte (255: unknown).
struct Header {
    Name    string
    Comment string
    ModTime int
    OS      int = 255
    Extra   []byte
}

// Compresses data into one member with an empty header.
func Compress(data []byte, level int) []byte {
    return CompressHeader(data, level, Header{})
}

// Compresses data into one member with header fields h.
func CompressHeader(data []byte, level int, h Header) []byte {
    var w = NewWriter(level)
    w.Header = h
    w.Write(data)
    w.Close()
    return w.Bytes()
}

// Decompresses all members of a gzip file.
func Decompress(data []byte) []byte! {
    var r = NewReader()
    r.Feed(data)?
    r.Close()?
    return r.Read()
}

// The header of the first member.
func ReadHeader(data []byte) Header! {
    var h = Header{}
    var n = parseHeader(data, &h)?
    if n < 0 {
        return error("gzip: unexpected end of header")
    }
    return h
}

func encodeHeader(h Header, level int) []byte {
    var flg = 0
    if len(h.Extra) > 0 {
        flg = flg | 4
    }
    if h.Name != "" {
        flg = flg | 8
    }
    if h.Comment != "" {
        flg = flg | 16
    }
    var b []byte = [byte(31), byte(139), byte(8), byte(flg)]
    b = std.compress.putLE32(b, h.ModTime)
    var xfl = 0
    if level == 9 {
        xfl = 2
    } else if level == 1 {
        xfl = 4
    }
    b = append(b, byte(xfl))
    b = append(b, byte(h.OS & 255))
    if len(h.Extra) > 0 {
        b = append(b, byte(len(h.Extra) & 255))
        b = append(b, byte((len(h.Extra) >> 8) & 255))
        b = std.compress.rtConcat(b, h.Extra)
    }
    if h.Name != "" {
        b = std.compress.rtConcat(b, std.strings.Bytes(h.Name))
        b = append(b, byte(0))
    }
    if h.Comment != "" {
        b = std.compress.rtConcat(b, std.strings.Bytes(h.Comment))
        b = append(b, byte(0))
    }
    return b
}

// Parses a member header at the start of b into h: its length, or -1 if
// b ends inside it.
func parseHeader(b []byte, h *Header) int! {
    if len(b) < 10 {
        if len(b) >= 1 && int(b[0]) != 31 || len(b) >= 2 && int(b[1]) != 139 {
            return error("gzip: invalid header")
        }
        return -1
    }
    if int(b[0]) != 31 || int(b[1]) != 139 {
        return error("gzip: invalid header")
    }
    if int(b[2]) != 8 {
        return error("gzip: unsupported compression method")
    }
    var flg = int(b[3])
    if (flg & 224) != 0 {
        return error("gzip: reserved header flags set")
    }
    h.ModTime = std.compress.le32(b, 4)
    h.OS = int(b[9])
    var i = 10
    if (flg & 4) != 0 {
        if len(b) < i + 2 {
            return -1
        }
        var xlen = int(b[i]) | (int(b[i + 1]) << 8)
        i += 2
        if len(b) < i + xlen {
            return -1
        }
        h.Extra = std.compress.rtCopy(b, i, i + xlen)
        i += xlen
    }
    if (flg & 8) != 0 {
        var j = zero(b, i)
        if j < 0 {
            return -1
        }
        h.Name = std.strings.FromBytes(std.compress.rtCopy(b, i, j))
        i = j + 1
    }
    if (flg & 16) != 0 {
        var j = zero(b, i)
        if j < 0 {
            return -1
        }
        h.Comment = std.strings.FromBytes(std.compress.rtCopy(b, i, j))
        i = j + 1
    }
    if (flg & 2) != 0 {
        if len(b) < i + 2 {
            return -1
        }
        var crc = std.compress.Crc32(0, std.compress.rtCopy(b, 0, i)) & 65535
        if crc != (int(b[i]) | (int(b[i + 1]) << 8)) {
            return error("gzip: header checksum mismatch")
        }
        i += 2
    }
    return i
}

func zero(b []byte, from int) int {
    for i in from..len(b) {
        if int(b[i]) == 0 {
            return i
        }
    }
    return -1
}

// A streaming gzip compressor: set Header before the first Write.
struct Writer {
    Header  Header
    d       *std.compress.deflate.Writer
    crc     int
    size    int
    out     []byte
    started bool
    closed  bool

    func Write(p []byte) {
        this.start()
        this.crc = std.compress.Crc32(this.crc, p)
        this.size += len(p)
        this.d.Write(p)
    }

    func WriteString(s string) {
        this.Write(std.strings.Bytes(s))
    }

    func Flush() {
        this.start()
        this.d.Flush()
    }

    func Close() {
        if this.closed {
            return
        }
        this.start()
        this.d.Close()
        this.out = std.compress.rtConcat(this.out, this.d.Bytes())
        this.out = std.compress.putLE32(this.out, this.crc)
        this.out = std.compress.putLE32(this.out, this.size & 4294967295)
        this.closed = true
    }

    // The compressed bytes produced since the last call.
    func Bytes() []byte {
        if !this.closed {
            this.out = std.compress.rtConcat(this.out, this.d.Bytes())
        }
        var b = this.out
        this.out = []
        return b
    }

    func start() {
        if !this.started {
            this.started = true
            this.out = encodeHeader(this.Header, this.d.level)
        }
    }
}

func NewWriter(level int) *Writer {
    return &Writer{d: std.compress.deflate.NewWriter(level)}
}

// A streaming gzip decompressor over concatenated members; checksums and
// sizes are verified at the end of each member.
struct Reader {
    state   int
    buf     []byte
    d       *std.compress.deflate.Reader
    crc     int
    size    int
    members int
    header  Header
    out     []byte

    func Feed(chunk []byte) void! {
        var input = chunk
        for {
            if this.state == 0 {
                this.buf = std.compress.rtConcat(this.buf, input)
                input = []
                if len(this.buf) == 0 {
                    return
                }
                var h = Header{}
                var n = parseHeader(this.buf, &h)?
                if n < 0 {
                    return
                }
                if this.members == 0 {
                    this.header = h
                }
                input = std.compress.rtCopy(this.buf, n, len(this.buf))
                this.buf = []
                this.d = std.compress.deflate.NewReader()
                this.crc = 0
                this.size = 0
                this.state = 1
            }
            if this.state == 1 {
                this.d.Feed(input)?
                input = []
                var b = this.d.Read()
                this.crc = std.compress.Crc32(this.crc, b)
                this.size += len(b)
                this.out = std.compress.rtConcat(this.out, b)
                if !this.d.Done() {
                    return
                }
                this.state = 2
                input = this.d.Rest()
            }
            this.buf = std.compress.rtConcat(this.buf, input)
            input = []
            if len(this.buf) < 8 {
                return
            }
            if std.compress.le32(this.buf, 0) != this.crc {
                return error("gzip: checksum mismatch")
            }
            if std.compress.le32(this.buf, 4) != (this.size & 4294967295) {
                return error("gzip: size mismatch")
            }
            this.members += 1
            input = std.compress.rtCopy(this.buf, 8, len(this.buf))
            this.buf = []
            this.state = 0
            if len(input) == 0 {
                return
            }
        }
    }

    // The decompressed bytes produced since the last call.
    func Read() []byte {
        var b = this.out
        this.out = []
        return b
    }

    // The header of the first member (nil before it was read).
    func Header() *Header {
        if this.members == 0 && this.state == 0 {
            return nil
        }
        var h = this.header
        return &h
    }

    // True when at least one member ended and no other one is in progress.
    func Done() bool {
        return this.members > 0 && this.state == 0 && len(this.buf) == 0
    }

    func Close() void! {
        if !this.Done() {
            return error("gzip: unexpected end of compressed data")
        }
    }
}

func NewReader() *Reader {
    return &Reader{}
}

// std.io adapters: gzip.NewStreamReader(src) is an io.Reader of the
// decompressed data of src; gzip.NewStreamWriter(dst, level) is an
// io.Writer that writes gzip data to dst (Close writes the trailer).
struct StreamReader {
    src     std.io.Reader
    z       *Reader
    pending []byte
    pos     int
    eof     bool

    func Read(buf []byte) int! {
        for this.pos >= len(this.pending) {
            if this.eof {
                return 0
            }
            var chunk = std.compress.rtZeros(32768)
            var n = this.src.Read(chunk)?
            if n == 0 {
                this.eof = true
                this.z.Close()?
            } else {
                this.z.Feed(std.compress.rtCopy(chunk, 0, n))?
            }
            this.pending = this.z.Read()
            this.pos = 0
        }
        var k = len(this.pending) - this.pos
        if k > len(buf) {
            k = len(buf)
        }
        for i in 0..k {
            buf[i] = this.pending[this.pos + i]
        }
        this.pos += k
        return k
    }

    // The header of the first member (nil before it was read).
    func Header() *Header {
        return this.z.Header()
    }
}

func NewStreamReader(src std.io.Reader) *StreamReader {
    return &StreamReader{src: src, z: NewReader()}
}

struct StreamWriter {
    dst std.io.Writer
    z   *Writer

    func Write(buf []byte) int! {
        this.z.Write(buf)
        var b = this.z.Bytes()
        if len(b) > 0 {
            std.io.WriteAll(this.dst, b)?
        }
        return len(buf)
    }

    func Flush() void! {
        this.z.Flush()
        std.io.WriteAll(this.dst, this.z.Bytes())?
    }

    func Close() void! {
        this.z.Close()
        std.io.WriteAll(this.dst, this.z.Bytes())?
    }

    func SetHeader(h Header) {
        this.z.Header = h
    }
}

func NewStreamWriter(dst std.io.Writer, level int) *StreamWriter {
    return &StreamWriter{dst: dst, z: NewWriter(level)}
}
