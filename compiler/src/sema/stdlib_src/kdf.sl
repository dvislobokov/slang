namespace std.crypto.kdf

// std.crypto.kdf: key derivation. Pbkdf2 (RFC 8018) and Scrypt (RFC 7914)
// are runtime functions (crypto/kdf.c); Argon2 (RFC 9106, version 0x13)
// runs in C as well (std_argon2.c); Hkdf wraps std.crypto.hkdf.
// Passwords and salts are []byte (std.strings.Bytes for strings); memory
// is in KiB.

// Argon2id(password, salt, time, memory KiB, threads, keyLen): the
// recommended password hash (RFC 9106 4: t=3, m=65536, p=4 or t=1,
// m=2097152, p=4). The salt must have at least 8 bytes (16 recommended).
func Argon2id(password []byte, salt []byte, time int, memory int, threads int, keyLen int) []byte! {
    var none []byte = []
    return rtArgon2(2, password, salt, none, none, time, memory, threads, keyLen)?
}

// Argon2i: data-independent memory access (side-channel resistant).
func Argon2i(password []byte, salt []byte, time int, memory int, threads int, keyLen int) []byte! {
    var none []byte = []
    return rtArgon2(1, password, salt, none, none, time, memory, threads, keyLen)?
}

// Argon2d: data-dependent memory access (fastest, for non-interactive use).
func Argon2d(password []byte, salt []byte, time int, memory int, threads int, keyLen int) []byte! {
    var none []byte = []
    return rtArgon2(0, password, salt, none, none, time, memory, threads, keyLen)?
}

// Full Argon2 parameters (Argon2 with a secret key K and associated data X).
struct Argon2Params {
    // "argon2id", "argon2i" or "argon2d".
    Variant string = "argon2id"
    Time int = 3
    // KiB.
    Memory int = 65536
    Threads int = 4
    KeyLen int = 32
    Secret []byte
    AssociatedData []byte
}

func variantCode(v string) int! {
    if v == "argon2id" || v == "" {
        return 2
    }
    if v == "argon2i" {
        return 1
    }
    if v == "argon2d" {
        return 0
    }
    return error("argon2: unknown variant " + v)
}

func Argon2(password []byte, salt []byte, p Argon2Params) []byte! {
    var t = variantCode(p.Variant)?
    return rtArgon2(t, password, salt, p.Secret, p.AssociatedData, p.Time, p.Memory, p.Threads, p.KeyLen)?
}

// HKDF (RFC 5869): Extract then Expand to keyLen bytes.
func Hkdf(hash string, secret []byte, salt []byte, info []byte, keyLen int) []byte! {
    var prk = std.crypto.hkdf.Extract(hash, salt, secret)?
    return std.crypto.hkdf.Expand(hash, prk, info, keyLen)?
}
