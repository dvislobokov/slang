// std.hash.crc64: CRC-64 with the ISO or ECMA polynomial (Go's
// hash/crc64): crc64.Checksum(data, crc64.MakeTable(crc64.ECMA)).
namespace std.hash.crc64

// A precomputed table for one polynomial (reversed form).
struct Table {
    t []uint
}

// The table of polynomial poly (crc64.ISO, crc64.ECMA or another
// reversed polynomial).
func MakeTable(poly uint) Table {
    var t []uint = []
    for i in 0..256 {
        var crc = uint(i)
        for _ in 0..8 {
            if (crc & 1) == 1 {
                crc = (crc >> 1) ^ poly
            } else {
                crc = crc >> 1
            }
        }
        t = append(t, crc)
    }
    return Table{t: t}
}

// Continues crc over p.
func Update(crc uint, tab Table, p []byte) uint {
    var c = crc ^ 18446744073709551615
    for b in p {
        c = tab.t[int((c ^ uint(b)) & 255)] ^ (c >> 8)
    }
    return c ^ 18446744073709551615
}

// The CRC-64 of data.
func Checksum(data []byte, tab Table) uint {
    return Update(0, tab, data)
}

// A streaming CRC-64 (std.hash.Hash64).
struct Digest {
    crc uint
    tab Table

    // Write adds p to the checksum; it never fails.
    func Write(p []byte) int! {
        this.crc = Update(this.crc, this.tab, p)
        return len(p)
    }

    // WriteString adds the bytes of s to the checksum.
    func WriteString(s string) int! {
        this.crc = Update(this.crc, this.tab, std.strings.Bytes(s))
        return len(s)
    }

    // Sum64 returns the checksum.
    func Sum64() uint {
        return this.crc
    }

    // Sum returns the checksum as 8 big-endian bytes.
    func Sum() []byte {
        return std.hash.Bytes64(this.crc)
    }

    // Reset restores the initial state.
    func Reset() {
        this.crc = 0
    }

    // Size returns 8.
    func Size() int {
        return 8
    }

    // BlockSize returns 1.
    func BlockSize() int {
        return 1
    }
}

// A new streaming CRC-64 with table tab.
func New(tab Table) Digest {
    return Digest{crc: 0, tab: tab}
}
