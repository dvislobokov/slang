namespace std.hash.crc32

// std.hash.crc32: CRC-32 with the IEEE, Castagnoli or Koopman polynomial
// (Go's hash/crc32). ChecksumIEEE matches std.crypto.hash.Crc32.

struct Table {
    t []uint32
}

// The table of reversed polynomial poly (crc32.IEEE, Castagnoli, Koopman).
func MakeTable(poly uint32) Table {
    var t []uint32 = []
    for i in 0..256 {
        var crc = uint32(i)
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

func Update(crc uint32, tab Table, p []byte) uint32 {
    var c = crc ^ 4294967295
    for b in p {
        c = tab.t[int((c ^ uint32(b)) & 255)] ^ (c >> 8)
    }
    return c ^ 4294967295
}

func Checksum(data []byte, tab Table) uint32 {
    return Update(0, tab, data)
}

// CRC-32 (IEEE) of data.
func ChecksumIEEE(data []byte) uint32 {
    return Update(0, MakeTable(0xEDB88320), data)
}

// A streaming CRC-32 (std.hash.Hash32).
struct Digest {
    crc uint32
    tab Table

    func Write(p []byte) int! {
        this.crc = Update(this.crc, this.tab, p)
        return len(p)
    }

    func WriteString(s string) int! {
        this.crc = Update(this.crc, this.tab, std.strings.Bytes(s))
        return len(s)
    }

    func Sum32() uint32 {
        return this.crc
    }

    func Sum() []byte {
        return std.hash.Bytes32(this.crc)
    }

    func Reset() {
        this.crc = 0
    }

    func Size() int {
        return 4
    }

    func BlockSize() int {
        return 1
    }
}

func New(tab Table) Digest {
    return Digest{crc: 0, tab: tab}
}

// A new streaming CRC-32 (IEEE).
func NewIEEE() Digest {
    return New(MakeTable(0xEDB88320))
}
