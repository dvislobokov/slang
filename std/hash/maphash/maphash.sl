namespace std.hash.maphash

// std.hash.maphash: fast seeded hashing of byte strings for hash tables
// (Go's hash/maphash). The function is SipHash-1-3 (C,
// runtime/src/std_hash.c): keyed, so attackers who do not know the seed
// cannot craft collisions. Not a cryptographic MAC.

// A random 128-bit key. Hashes are only comparable under the same Seed.
struct Seed {
    k0 uint
    k1 uint
}

// A new random seed (from the system CSPRNG).
func MakeSeed() Seed {
    var b = try std.crypto.rand.Bytes(16) catch err {
        panic(err)
    }
    return SeedFrom(b)
}

// A fixed seed from 16 bytes (for reproducible hashes, e.g. in tests).
func SeedFrom(b []byte) Seed {
    if len(b) < 16 {
        panic("maphash.SeedFrom: need 16 bytes")
    }
    var k0 uint = 0
    var k1 uint = 0
    for i in 0..8 {
        k0 = k0 | (uint(b[i]) << uint(8 * i))
        k1 = k1 | (uint(b[8 + i]) << uint(8 * i))
    }
    return Seed{k0: k0, k1: k1}
}

// The hash of b under seed.
func Bytes(seed Seed, b []byte) uint {
    return rtSip13(seed.k0, seed.k1, b)
}

// The hash of s under seed.
func String(seed Seed, s string) uint {
    return rtSip13Str(seed.k0, seed.k1, s)
}

// A streaming maphash (std.hash.Hash64): buffers the input and hashes it
// on Sum64, so the result equals Bytes(seed, everything written).
struct Hash {
    seed   Seed
    buf    []byte
    seeded bool

    // Sets the seed (otherwise a random one is chosen on first use).
    func SetSeed(s Seed) {
        this.seed = s
        this.seeded = true
        this.buf = nil
    }

    func Seed() Seed {
        this.init()
        return this.seed
    }

    func init() {
        if !this.seeded {
            this.seed = MakeSeed()
            this.seeded = true
        }
    }

    func Write(p []byte) int! {
        for c in p {
            this.buf = append(this.buf, c)
        }
        return len(p)
    }

    func WriteString(s string) int! {
        for i in 0..len(s) {
            this.buf = append(this.buf, s[i])
        }
        return len(s)
    }

    func Sum64() uint {
        this.init()
        return Bytes(this.seed, this.buf)
    }

    func Sum() []byte {
        return std.hash.Bytes64(this.Sum64())
    }

    func Reset() {
        this.buf = nil
    }

    func Size() int {
        return 8
    }

    func BlockSize() int {
        return 8
    }
}
