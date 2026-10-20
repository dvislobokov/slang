namespace std.encoding.asn1

// std.encoding.asn1: a DER reader and writer (the subset X.509, PKCS and
// most protocols use). The element scanner and OID decoder are the ones
// std.crypto.x509 uses (its reader in x509.sl); this module adds the
// public typed API on top: INTEGER (int and big.Int), BOOLEAN, NULL,
// OBJECT IDENTIFIER, OCTET STRING, BIT STRING, UTF8/Printable/IA5/
// Numeric/T61/BMP strings, UTCTime, GeneralizedTime, SEQUENCE, SET and
// context-specific [n] tags; and a Builder that writes the same.

// One encoded element. Tag is the whole identifier byte (class bits,
// constructed bit and tag number < 31), as in x509.
struct RawValue {
    Tag int
    Bytes []byte
    FullBytes []byte

    // 0 universal, 1 application, 2 context-specific, 3 private.
    func Class() int {
        return this.Tag >> 6
    }

    func Constructed() bool {
        return (this.Tag & 32) != 0
    }

    // The tag number (low 5 bits).
    func Number() int {
        return this.Tag & 31
    }
}

// A BIT STRING: Bytes with UnusedBits (0..7) unused low bits in the last byte.
struct BitString {
    Bytes []byte
    UnusedBits int

    // Number of bits.
    func Len() int {
        return len(this.Bytes) * 8 - this.UnusedBits
    }

    // Bit i (0 = most significant bit of the first byte).
    func At(i int) int {
        if i < 0 || i >= this.Len() {
            return 0
        }
        return int((this.Bytes[i / 8] >> byte(7 - i % 8)) & 1)
    }
}

// Reads consecutive DER elements.
struct Reader {
    r *std.crypto.x509.reader

    // True if elements remain.
    func More() bool {
        return this.r.more()
    }

    // The identifier byte of the next element (-1 at the end).
    func PeekTag() int {
        return this.r.peek()
    }

    // The next element, whatever its type.
    func Next() RawValue! {
        var e = this.r.next()?
        return RawValue{Tag: e.tag, Bytes: e.body, FullBytes: e.full}
    }

    func expect(tag int, what string) RawValue! {
        var v = this.Next()?
        if v.Tag != tag {
            return error("asn1: " + what + ": want tag 0x" + std.strconv.FormatInt(tag, 16) + ", found 0x" + std.strconv.FormatInt(v.Tag, 16))
        }
        return v
    }

    // A SEQUENCE: a Reader over its contents.
    func ReadSequence() Reader! {
        var v = this.expect(0x30, "SEQUENCE")?
        return NewReader(v.Bytes)
    }

    // A SET: a Reader over its contents.
    func ReadSet() Reader! {
        var v = this.expect(0x31, "SET")?
        return NewReader(v.Bytes)
    }

    // An explicit context-specific tag [n] (constructed): its contents.
    func ReadExplicit(n int) Reader! {
        var v = this.expect(0xA0 | n, "[" + std.strconv.Itoa(n) + "]")?
        return NewReader(v.Bytes)
    }

    // True (and consumes nothing) if the next element is [n] explicit.
    func HasExplicit(n int) bool {
        return this.r.peek() == (0xA0 | n)
    }

    // An implicit context-specific primitive tag [n]: its content bytes.
    func ReadImplicit(n int) []byte! {
        var v = this.expect(0x80 | n, "[" + std.strconv.Itoa(n) + "] IMPLICIT")?
        return v.Bytes
    }

    func ReadBool() bool! {
        var v = this.expect(1, "BOOLEAN")?
        if len(v.Bytes) != 1 || (v.Bytes[0] != 0 && v.Bytes[0] != 255) {
            return error("asn1: invalid BOOLEAN")
        }
        return v.Bytes[0] == 255
    }

    func ReadNull() void! {
        var v = this.expect(5, "NULL")?
        if len(v.Bytes) != 0 {
            return error("asn1: invalid NULL")
        }
    }

    // An INTEGER that fits in int.
    func ReadInt() int! {
        var v = this.expect(2, "INTEGER")?
        checkInt(v.Bytes)?
        if len(v.Bytes) > 8 {
            return error("asn1: INTEGER too large for int")
        }
        var n = 0
        if v.Bytes[0] >= 128 {
            n = -1
        }
        for b in v.Bytes {
            n = (n << 8) | int(b)
        }
        return n
    }

    // An ENUMERATED value.
    func ReadEnum() int! {
        var v = this.expect(10, "ENUMERATED")?
        checkInt(v.Bytes)?
        var n = 0
        if v.Bytes[0] >= 128 {
            n = -1
        }
        for b in v.Bytes {
            n = (n << 8) | int(b)
        }
        return n
    }

    // An INTEGER of any size (two's complement).
    func ReadBigInt() std.math.big.Int! {
        var v = this.expect(2, "INTEGER")?
        checkInt(v.Bytes)?
        var mag = std.math.big.FromBytes(v.Bytes)
        if v.Bytes[0] >= 128 {
            // negative: value = mag - 2^(8n)
            return mag.Sub(std.math.big.NewInt(1).Lsh(8 * len(v.Bytes)))
        }
        return mag
    }

    // An OBJECT IDENTIFIER in dotted form ("1.2.840.113549").
    func ReadOID() string! {
        var v = this.expect(6, "OBJECT IDENTIFIER")?
        return std.crypto.x509.oidString(v.Bytes)?
    }

    func ReadOctetString() []byte! {
        var v = this.expect(4, "OCTET STRING")?
        return v.Bytes
    }

    func ReadBitString() BitString! {
        var v = this.expect(3, "BIT STRING")?
        if len(v.Bytes) == 0 || v.Bytes[0] > 7 || (len(v.Bytes) == 1 && v.Bytes[0] != 0) {
            return error("asn1: invalid BIT STRING")
        }
        return BitString{Bytes: v.Bytes[1..], UnusedBits: int(v.Bytes[0])}
    }

    // Any of the character string types, as UTF-8 text.
    func ReadString() string! {
        var v = this.Next()?
        switch v.Tag {
            case 12:
                if !std.unicode.utf8.Valid(v.Bytes) {
                    return error("asn1: invalid UTF8String")
                }
                return std.strings.FromBytes(v.Bytes)
            case 19, 22, 18, 20, 26:
                for b in v.Bytes {
                    if b >= 128 && v.Tag != 20 {
                        return error("asn1: invalid character in string")
                    }
                }
                if v.Tag == 20 {
                    // T61: treat as Latin-1
                    var rs []char = []
                    for b in v.Bytes {
                        rs = append(rs, char(int(b)))
                    }
                    return std.strings.FromRunes(rs)
                }
                return std.strings.FromBytes(v.Bytes)
            case 30:
                if len(v.Bytes) % 2 != 0 {
                    return error("asn1: invalid BMPString")
                }
                var u []uint16 = []
                var i = 0
                for i < len(v.Bytes) {
                    u = append(u, (uint16(v.Bytes[i]) << 8) | uint16(v.Bytes[i + 1]))
                    i += 2
                }
                return std.strings.FromRunes(std.unicode.utf16.Decode(u))
        }
        return error("asn1: not a string type: tag 0x" + std.strconv.FormatInt(v.Tag, 16))
    }

    // A UTCTime or GeneralizedTime, in UTC.
    func ReadTime() std.time.Time! {
        var v = this.Next()?
        var s = std.strings.FromBytes(v.Bytes)
        if v.Tag == 23 {
            var t = try std.time.ParseTime("060102150405Z0700", s) catch err {
                return error("asn1: invalid UTCTime " + std.strconv.Quote(s))
            }
            // RFC 5280: YY >= 50 means 19YY
            var y = t.Year()
            if y >= 2050 {
                t = t.AddDate(-100, 0, 0)
            }
            return t.UTC()
        }
        if v.Tag == 24 {
            var layout = "20060102150405Z0700"
            if std.strings.Contains(s, ".") {
                layout = "20060102150405.999999999Z0700"
            }
            var t = try std.time.ParseTime(layout, s) catch err {
                return error("asn1: invalid GeneralizedTime " + std.strconv.Quote(s))
            }
            return t.UTC()
        }
        return error("asn1: not a time: tag 0x" + std.strconv.FormatInt(v.Tag, 16))
    }
}

func checkInt(b []byte) void! {
    if len(b) == 0 {
        return error("asn1: empty INTEGER")
    }
    if len(b) > 1 && ((b[0] == 0 && b[1] < 128) || (b[0] == 255 && b[1] >= 128)) {
        return error("asn1: non-minimal INTEGER")
    }
}

// A Reader over the DER bytes b.
func NewReader(b []byte) Reader {
    return Reader{r: std.crypto.x509.newReader(b)}
}

// Writes DER: values are appended in order; AddSequence/AddSet/
// AddExplicit nest through a callback.
struct Builder {
    buf []byte

    // The encoding so far.
    func Bytes() []byte {
        return this.buf
    }

    // Appends tag, length and content.
    func AddTagged(tag int, content []byte) {
        this.buf = append(this.buf, byte(tag))
        var n = len(content)
        if n < 128 {
            this.buf = append(this.buf, byte(n))
        } else {
            var lb []byte = []
            var x = n
            for x > 0 {
                lb = append(lb, byte(x & 255))
                x = x >> 8
            }
            this.buf = append(this.buf, byte(128 | len(lb)))
            var i = len(lb) - 1
            for i >= 0 {
                this.buf = append(this.buf, lb[i])
                i -= 1
            }
        }
        for c in content {
            this.buf = append(this.buf, c)
        }
    }

    // Appends an already encoded element.
    func AddRaw(der []byte) {
        for c in der {
            this.buf = append(this.buf, c)
        }
    }

    func AddBool(v bool) {
        if v {
            this.AddTagged(1, [byte(255)])
        } else {
            this.AddTagged(1, [byte(0)])
        }
    }

    func AddNull() {
        var empty []byte = []
        this.AddTagged(5, empty)
    }

    func AddInt(v int) {
        this.AddTagged(2, intBytes(v))
    }

    func AddEnum(v int) {
        this.AddTagged(10, intBytes(v))
    }

    func AddBigInt(v std.math.big.Int) {
        if v.Sign() >= 0 {
            var b = v.Bytes()
            if len(b) == 0 || b[0] >= 128 {
                var z []byte = [byte(0)]
                for c in b {
                    z = append(z, c)
                }
                b = z
            }
            this.AddTagged(2, b)
            return
        }
        // two's complement of a negative value: 2^(8n) + v with n minimal
        var n = (v.Abs().Sub(std.math.big.NewInt(1)).BitLen() + 8) / 8
        var tc = std.math.big.NewInt(1).Lsh(8 * n).Add(v).Bytes()
        for len(tc) < n {
            var z []byte = [byte(0)]
            for c in tc {
                z = append(z, c)
            }
            tc = z
        }
        this.AddTagged(2, tc)
    }

    // A dotted OID; error if it is malformed.
    func AddOID(oid string) void! {
        var parts = std.strings.Split(oid, ".")
        if len(parts) < 2 {
            return error("asn1: invalid OID " + std.strconv.Quote(oid))
        }
        var nums []int = []
        for p in parts {
            nums = append(nums, std.strconv.Atoi(p)?("asn1: invalid OID " + std.strconv.Quote(oid)))
        }
        if nums[0] > 2 || (nums[0] < 2 && nums[1] >= 40) {
            return error("asn1: invalid OID " + std.strconv.Quote(oid))
        }
        var body []byte = []
        body = base128(body, nums[0] * 40 + nums[1])
        for i in 2..len(nums) {
            body = base128(body, nums[i])
        }
        this.AddTagged(6, body)
    }

    func AddOctetString(b []byte) {
        this.AddTagged(4, b)
    }

    func AddBitString(b BitString) {
        var body = [byte(b.UnusedBits)]
        for c in b.Bytes {
            body = append(body, c)
        }
        this.AddTagged(3, body)
    }

    func AddUTF8String(s string) {
        this.AddTagged(12, std.strings.Bytes(s))
    }

    func AddPrintableString(s string) {
        this.AddTagged(19, std.strings.Bytes(s))
    }

    func AddIA5String(s string) {
        this.AddTagged(22, std.strings.Bytes(s))
    }

    // UTCTime (years 1950..2049) "YYMMDDHHMMSSZ".
    func AddUTCTime(t std.time.Time) {
        this.AddTagged(23, std.strings.Bytes(t.UTC().Format("060102150405Z")))
    }

    // GeneralizedTime "YYYYMMDDHHMMSSZ" (fraction if non-zero).
    func AddGeneralizedTime(t std.time.Time) {
        this.AddTagged(24, std.strings.Bytes(t.UTC().Format("20060102150405.999999999Z")))
    }

    // A SEQUENCE whose contents fill writes.
    func AddSequence(fill func(*Builder)) {
        var inner = new Builder{}
        fill(inner)
        this.AddTagged(0x30, inner.buf)
    }

    func AddSet(fill func(*Builder)) {
        var inner = new Builder{}
        fill(inner)
        this.AddTagged(0x31, inner.buf)
    }

    // An explicit context-specific [n] around what fill writes.
    func AddExplicit(n int, fill func(*Builder)) {
        var inner = new Builder{}
        fill(inner)
        this.AddTagged(0xA0 | n, inner.buf)
    }
}

func base128(b []byte, v int) []byte {
    var groups []byte = []
    var x = v
    groups = append(groups, byte(x & 127))
    x = x >> 7
    for x > 0 {
        groups = append(groups, byte((x & 127) | 128))
        x = x >> 7
    }
    var out = b
    var i = len(groups) - 1
    for i >= 0 {
        out = append(out, groups[i])
        i -= 1
    }
    return out
}

// Minimal two's complement bytes of v.
func intBytes(v int) []byte {
    var out []byte = []
    var shift = 56
    for shift >= 0 {
        out = append(out, byte((v >> shift) & 255))
        shift -= 8
    }
    // strip redundant leading 0x00 / 0xFF
    var i = 0
    for i < 7 && ((out[i] == 0 && out[i + 1] < 128) || (out[i] == 255 && out[i + 1] >= 128)) {
        i += 1
    }
    return out[i..]
}

// A new, empty Builder.
func NewBuilder() *Builder {
    return new Builder{}
}
