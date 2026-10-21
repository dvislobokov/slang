namespace std.encoding.base32

// std.encoding.base32: RFC 4648 base32 (Go's encoding/base32). Encode /
// Decode use the standard alphabet with padding, like std.encoding.base64;
// EncodeHex / DecodeHex the "extended hex" alphabet; NewEncoding makes
// others, with or without padding.

// An alphabet of 32 distinct ASCII characters and a padding choice.
struct Encoding {
    alphabet string
    pad      bool

    // The same alphabet with (true) or without (false) '=' padding.
    func WithPadding(pad bool) Encoding {
        return Encoding{alphabet: this.alphabet, pad: pad}
    }

    func Encode(src []byte) string {
        var out []byte = []
        var i = 0
        for i < len(src) {
            var n = len(src) - i
            if n > 5 {
                n = 5
            }
            var buf uint = 0
            for k in 0..5 {
                buf = buf << 8
                if k < n {
                    buf = buf | uint(src[i + k])
                }
            }
            // n bytes -> ceil(8n/5) symbols
            var symbols = (8 * n + 4) / 5
            for k in 0..8 {
                if k < symbols {
                    out = append(out, this.alphabet[int((buf >> uint(35 - 5 * k)) & 31)])
                } else if this.pad {
                    out = append(out, 61)
                }
            }
            i += 5
        }
        return std.strings.FromBytes(out)
    }

    func EncodeString(s string) string {
        return this.Encode(std.strings.Bytes(s))
    }

    // Decodes s; padding is required iff the encoding pads. Line breaks
    // (\r, \n) are ignored, as in Go.
    func Decode(s string) []byte! {
        var vals []int = []
        var padding = 0
        for i in 0..len(s) {
            var c = s[i]
            if c == 10 || c == 13 {
                continue
            }
            if c == 61 {
                padding += 1
                continue
            }
            if padding > 0 {
                return error("base32: illegal data at input byte " + std.strconv.Itoa(i))
            }
            var v = std.strings.Index(this.alphabet, std.strings.FromBytes([c]))
            if v < 0 {
                return error("base32: illegal data at input byte " + std.strconv.Itoa(i))
            }
            vals = append(vals, v)
        }
        var total = len(vals) + padding
        if this.pad && total % 8 != 0 {
            return error("base32: illegal data at input byte " + std.strconv.Itoa(len(s)))
        }
        var rem = len(vals) % 8
        if rem == 1 || rem == 3 || rem == 6 {
            return error("base32: illegal data at input byte " + std.strconv.Itoa(len(s)))
        }
        var out []byte = []
        var i = 0
        for i < len(vals) {
            var n = len(vals) - i
            if n > 8 {
                n = 8
            }
            var buf uint = 0
            for k in 0..8 {
                buf = buf << 5
                if k < n {
                    buf = buf | uint(vals[i + k])
                }
            }
            var bytes = (5 * n) / 8
            for k in 0..bytes {
                out = append(out, byte((buf >> uint(32 - 8 * k)) & 255))
            }
            i += 8
        }
        return out
    }

    func DecodeString(s string) string! {
        return std.strings.FromBytes(this.Decode(s)?)
    }
}

// An encoding with the given 32-character alphabet, padded.
func NewEncoding(alphabet string) Encoding {
    if len(alphabet) != 32 {
        panic("base32: encoding alphabet is not 32 bytes long")
    }
    return Encoding{alphabet: alphabet, pad: true}
}

// The standard alphabet A-Z2-7 (padded).
func Std() Encoding {
    return NewEncoding("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")
}

// The extended hex alphabet 0-9A-V (padded).
func Hex() Encoding {
    return NewEncoding("0123456789ABCDEFGHIJKLMNOPQRSTUV")
}

func Encode(src []byte) string {
    return Std().Encode(src)
}

func EncodeString(s string) string {
    return Std().EncodeString(s)
}

func Decode(s string) []byte! {
    return Std().Decode(s)?
}

func DecodeString(s string) string! {
    return Std().DecodeString(s)?
}

func EncodeHex(src []byte) string {
    return Hex().Encode(src)
}

func DecodeHex(s string) []byte! {
    return Hex().Decode(s)?
}
