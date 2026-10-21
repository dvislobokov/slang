// Raw DEFLATE (RFC 1951) streams. The coder is C (runtime/src/std_compress.c,
// reached through the std.compress rt* externs); this file is the API:
// one-shot Compress/Decompress and the streaming Writer/Reader, which work
// on []byte chunks of any size.
namespace std.compress.deflate

// Compresses data at level 0 (store) .. 9 (best); -1 is the default (6).
func Compress(data []byte, level int) []byte {
    var none []byte = []
    return std.compress.rtDeflate(none, data, std.compress.level(level), true)
}

// Decompresses a complete raw DEFLATE stream. Bytes after its end are an error.
func Decompress(data []byte) []byte! {
    var r = NewReader()
    r.Feed(data)?
    if !r.Done() {
        return error("deflate: unexpected end of compressed data")
    }
    if len(r.Rest()) > 0 {
        return error("deflate: data after the end of the compressed stream")
    }
    return r.Read()
}

// A streaming compressor: Write plain bytes, take the compressed output
// with Bytes() whenever convenient; Flush makes everything written so far
// decodable (sync flush); Close ends the stream.
struct Writer {
    level   int
    pending []byte
    hist    []byte
    out     []byte
    closed  bool

    func Write(p []byte) {
        if this.closed {
            panic("deflate: Write after Close")
        }
        this.pending = std.compress.rtConcat(this.pending, p)
        if len(this.pending) >= 131072 {
            this.compress(false)
        }
    }

    func WriteString(s string) {
        this.Write(std.strings.Bytes(s))
    }

    // Compresses the buffered input; the output is byte-aligned afterwards.
    func Flush() {
        if !this.closed {
            this.compress(false)
        }
    }

    // Ends the stream (further Writes panic); Close twice is a no-op.
    func Close() {
        if !this.closed {
            this.compress(true)
            this.closed = true
        }
    }

    // The compressed bytes produced since the last call.
    func Bytes() []byte {
        var b = this.out
        this.out = []
        return b
    }

    func compress(final bool) {
        var c = std.compress.rtDeflate(this.hist, this.pending, this.level, final)
        this.out = std.compress.rtConcat(this.out, c)
        // keep the last 32 KB as history for the next chunk
        var h = std.compress.rtConcat(this.hist, this.pending)
        if len(h) > 32768 {
            h = std.compress.rtCopy(h, len(h) - 32768, len(h))
        }
        this.hist = h
        this.pending = []
    }
}

func NewWriter(level int) *Writer {
    return &Writer{level: std.compress.level(level)}
}

// A streaming decompressor: Feed compressed chunks as they arrive, take
// the decompressed bytes with Read(). Done() reports the end of the stream;
// Rest() holds the input bytes that followed it.
struct Reader {
    st   []byte
    out  []byte
    rest []byte
    done bool

    // Decompresses a chunk of input; fails on corrupt data.
    func Feed(chunk []byte) void! {
        if this.done {
            this.rest = std.compress.rtConcat(this.rest, chunk)
            return
        }
        var b = std.compress.rtInflate(this.st, chunk)?
        this.out = std.compress.rtConcat(this.out, b)
        if std.compress.rtInflateDone(this.st) {
            this.done = true
            var unused = std.compress.rtInflateUnused(this.st)
            var tail = std.compress.rtInflateTail(this.st)
            this.rest = std.compress.rtConcat(
                tail,
                std.compress.rtCopy(chunk, len(chunk) - unused, len(chunk)),
            )
        }
    }

    // The decompressed bytes produced since the last call.
    func Read() []byte {
        var b = this.out
        this.out = []
        return b
    }

    func Done() bool {
        return this.done
    }

    // The input after the end of the stream (a trailer, the next member...).
    func Rest() []byte {
        return this.rest
    }

    // Fails if the stream did not end.
    func Close() void! {
        if !this.done {
            return error("deflate: unexpected end of compressed data")
        }
    }
}

func NewReader() *Reader {
    return &Reader{st: std.compress.rtInflateNew()}
}
