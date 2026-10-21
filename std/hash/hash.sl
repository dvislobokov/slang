namespace std.hash

// std.hash: the streaming hash interfaces (Go's hash.Hash, Hash32,
// Hash64) implemented by std.hash.fnv, crc32, crc64, adler32 and
// maphash, plus Combine for building hash keys from several values.

// A streaming hash: Write data (never fails), then Sum.
interface Hasher {
    func Write(p []byte) int!
    // Default: Write of the string's bytes (implementations may override).
    func WriteString(s string) int! {
        return this.Write(std.strings.Bytes(s))?
    }
    // The hash of everything written so far (big-endian bytes); does not
    // change the state.
    func Sum() []byte
    func Reset()
    // Number of bytes Sum returns.
    func Size() int
    func BlockSize() int
}

// A Hasher with a 32-bit result.
interface Hash32 {
    func Write(p []byte) int!
    // Default: Write of the string's bytes (implementations may override).
    func WriteString(s string) int! {
        return this.Write(std.strings.Bytes(s))?
    }
    func Sum() []byte
    func Reset()
    func Size() int
    func BlockSize() int
    func Sum32() uint32
}

// A Hasher with a 64-bit result.
interface Hash64 {
    func Write(p []byte) int!
    // Default: Write of the string's bytes (implementations may override).
    func WriteString(s string) int! {
        return this.Write(std.strings.Bytes(s))?
    }
    func Sum() []byte
    func Reset()
    func Size() int
    func BlockSize() int
    func Sum64() uint
}

// Mixes hash h2 into h1 (boost::hash_combine, 64-bit), for hashing
// composite keys: Combine(Combine(seed, hash(a)), hash(b)).
func Combine(h1 uint, h2 uint) uint {
    return h1 ^ (h2 + 0x9E3779B97F4A7C15 + (h1 << 6) + (h1 >> 2))
}

// Big-endian bytes of x (for Sum of 32-bit hashes).
func Bytes32(x uint32) []byte {
    return [byte(x >> 24), byte((x >> 16) & 255), byte((x >> 8) & 255), byte(x & 255)]
}

// Big-endian bytes of x (for Sum of 64-bit hashes).
func Bytes64(x uint) []byte {
    var out []byte = []
    var shift = 56
    for shift >= 0 {
        out = append(out, byte((x >> uint(shift)) & 255))
        shift -= 8
    }
    return out
}
