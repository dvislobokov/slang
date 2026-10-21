// std.compress: shared helpers of the format namespaces (deflate, zlib,
// gzip, zstd). The level constants NoCompression, BestSpeed,
// BestCompression and DefaultCompression are compiler constants
// (sema/stdlib.rs); the rt* externs are runtime/src/std_compress.c and
// std_zstd.c.
namespace std.compress

// A level argument: -1 (default) becomes 6; out of range panics.
func level(l int) int {
    if l == -1 {
        return 6
    }
    if l < 0 || l > 9 {
        panic(std.fmt.Format("compress: invalid level {0} (want -1 or 0..9)", l))
    }
    return l
}

// Little- and big-endian helpers for the container formats.
func le32(b []byte, i int) int {
    return int(b[i]) | (int(b[i + 1]) << 8) | (int(b[i + 2]) << 16) | (int(b[i + 3]) << 24)
}

func be32(b []byte, i int) int {
    return (int(b[i]) << 24) | (int(b[i + 1]) << 16) | (int(b[i + 2]) << 8) | int(b[i + 3])
}

func putLE32(b []byte, v int) []byte {
    var out = b
    for k in 0..4 {
        out = append(out, byte((v >> (8 * k)) & 255))
    }
    return out
}

func putBE32(b []byte, v int) []byte {
    var out = b
    var k = 3
    for k >= 0 {
        out = append(out, byte((v >> (8 * k)) & 255))
        k -= 1
    }
    return out
}

// CRC-32 (IEEE) of data; crc continues an earlier result (0 to start).
func Crc32(crc int, data []byte) int {
    return rtCrc32(crc, data)
}

// Adler-32 of data; adler continues an earlier result (1 to start).
func Adler32(adler int, data []byte) int {
    return rtAdler32(adler, data)
}
