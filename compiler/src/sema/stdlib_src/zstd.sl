namespace std.compress.zstd

// Zstandard (RFC 8878) decoding; the decoder is C (runtime/src/std_zstd.c).
// Frames are decoded whole: the streaming Reader emits a frame's content
// once all of it has arrived. Dictionaries are not supported; there is no
// encoder yet.

// Decompresses all frames of data (skippable frames are skipped).
func Decompress(data []byte) []byte! {
    return std.compress.rtZstdDecode(data)?
}

// Reports whether data starts with a zstd frame magic number.
func IsFrame(data []byte) bool {
    return len(data) >= 4 && int(data[0]) == 40 && int(data[1]) == 181 && int(data[2]) == 47 && int(data[3]) == 253
}

// A streaming decompressor: Feed chunks; Read returns the content of the
// frames completed so far.
struct Reader {
    buf []byte
    out []byte
    frames int

    func Feed(chunk []byte) void! {
        this.buf = std.compress.rtConcat(this.buf, chunk)
        for len(this.buf) > 0 {
            var n = std.compress.rtZstdFrameLen(this.buf)
            if n < 0 {
                return error("zstd: invalid magic number")
            }
            if n == 0 {
                return
            }
            var b = std.compress.rtZstdDecode(std.compress.rtCopy(this.buf, 0, n))?
            this.out = std.compress.rtConcat(this.out, b)
            this.buf = std.compress.rtCopy(this.buf, n, len(this.buf))
            this.frames += 1
        }
    }

    // The decompressed bytes produced since the last call.
    func Read() []byte {
        var b = this.out
        this.out = []
        return b
    }

    // True when at least one frame was decoded and no other is in progress.
    func Done() bool {
        return this.frames > 0 && len(this.buf) == 0
    }

    func Close() void! {
        if !this.Done() {
            return error("zstd: unexpected end of data")
        }
    }
}

func NewReader() *Reader {
    return &Reader{}
}
