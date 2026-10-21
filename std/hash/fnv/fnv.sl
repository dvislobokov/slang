namespace std.hash.fnv

// std.hash.fnv: FNV-1 and FNV-1a, 32 and 64 bit (Go's hash/fnv). Each
// New* returns a value implementing std.hash.Hash32 / Hash64.

struct Hash32 {
    h uint32
    a bool

    func Write(p []byte) int! {
        var h = this.h
        var prime uint32 = 16777619
        for c in p {
            if this.a {
                h = (h ^ uint32(c)) * prime
            } else {
                h = (h * prime) ^ uint32(c)
            }
        }
        this.h = h
        return len(p)
    }

    func WriteString(s string) int! {
        var h = this.h
        var prime uint32 = 16777619
        for i in 0..len(s) {
            if this.a {
                h = (h ^ uint32(s[i])) * prime
            } else {
                h = (h * prime) ^ uint32(s[i])
            }
        }
        this.h = h
        return len(s)
    }

    func Sum32() uint32 {
        return this.h
    }

    func Sum() []byte {
        return std.hash.Bytes32(this.h)
    }

    func Reset() {
        this.h = 2166136261
    }

    func Size() int {
        return 4
    }

    func BlockSize() int {
        return 1
    }
}

struct Hash64 {
    h uint
    a bool

    func Write(p []byte) int! {
        var h = this.h
        var prime uint = 1099511628211
        for c in p {
            if this.a {
                h = (h ^ uint(c)) * prime
            } else {
                h = (h * prime) ^ uint(c)
            }
        }
        this.h = h
        return len(p)
    }

    func WriteString(s string) int! {
        var h = this.h
        var prime uint = 1099511628211
        for i in 0..len(s) {
            if this.a {
                h = (h ^ uint(s[i])) * prime
            } else {
                h = (h * prime) ^ uint(s[i])
            }
        }
        this.h = h
        return len(s)
    }

    func Sum64() uint {
        return this.h
    }

    func Sum() []byte {
        return std.hash.Bytes64(this.h)
    }

    func Reset() {
        this.h = 14695981039346656037
    }

    func Size() int {
        return 8
    }

    func BlockSize() int {
        return 1
    }
}

// FNV-1, 32-bit.
func New32() Hash32 {
    return Hash32{h: 2166136261, a: false}
}

// FNV-1a, 32-bit.
func New32a() Hash32 {
    return Hash32{h: 2166136261, a: true}
}

// FNV-1, 64-bit.
func New64() Hash64 {
    return Hash64{h: 14695981039346656037, a: false}
}

// FNV-1a, 64-bit.
func New64a() Hash64 {
    return Hash64{h: 14695981039346656037, a: true}
}

// FNV-1a 64 of s in one call.
func String64a(s string) uint {
    var h = New64a()
    _ = h.WriteString(s)
    return h.Sum64()
}
