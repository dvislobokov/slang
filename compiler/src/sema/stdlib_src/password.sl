namespace std.crypto.password

// std.crypto.password: password hashing in the PHC string format with
// Argon2id (RFC 9106). Hash picks a random 16-byte salt;
// Verify recomputes the hash with the parameters stored in the string and
// compares in constant time; NeedsRehash tells whether a stored hash uses
// parameters other than the current defaults.
//
//   $argon2id$v=19$m=65536,t=3,p=4$<salt, base64 without padding>$<hash>

// Argon2id parameters (defaults: RFC 9106 4, second recommended option).
struct Params {
    Time int = 3
    // KiB.
    Memory  int = 65536
    Threads int = 4
    KeyLen  int = 32
    SaltLen int = 16
}

// The default parameters.
func DefaultParams() Params {
    var p Params
    return p
}

// Hashes pw with the default parameters.
func Hash(pw string) string! {
    return HashWith(pw, DefaultParams())?
}

func HashWith(pw string, p Params) string! {
    if p.SaltLen < 8 {
        return error("password: salt length must be at least 8")
    }
    var salt = std.crypto.rand.Bytes(p.SaltLen)?
    var key = std.crypto.kdf.Argon2id(
        std.strings.Bytes(pw),
        salt,
        p.Time,
        p.Memory,
        p.Threads,
        p.KeyLen,
    )?
    return "$argon2id$v=19$m=" +
        std.fmt.Format("{0}", p.Memory) +
        ",t=" +
        std.fmt.Format("{0}", p.Time) +
        ",p=" +
        std.fmt.Format("{0}", p.Threads) +
        "$" +
        b64(salt) +
        "$" +
        b64(key)
}

func b64(b []byte) string {
    var s = std.encoding.base64.Encode(b)
    for std.strings.HasSuffix(s, "=") {
        s = s[..len(s) - 1]
    }
    return s
}

func unb64(s string) []byte! {
    var t = s
    for len(t) % 4 != 0 {
        t = t + "="
    }
    return std.encoding.base64.Decode(t)?
}

// A parsed PHC string.
struct Info {
    // "argon2id", "argon2i" or "argon2d".
    Variant string
    Version int
    Params  Params
    Salt    []byte
    Key     []byte
}

// Parses a PHC-format Argon2 hash.
func Parse(hash string) Info! {
    var parts = std.strings.Split(hash, "$")
    if len(parts) != 6 || parts[0] != "" {
        return error("password: malformed hash")
    }
    var info = Info{Variant: parts[1]}
    if info.Variant != "argon2id" && info.Variant != "argon2i" && info.Variant != "argon2d" {
        return error("password: unsupported algorithm " + info.Variant)
    }
    if !std.strings.HasPrefix(parts[2], "v=") {
        return error("password: malformed version")
    }
    info.Version = std.strconv.Atoi(parts[2][2..])?("password: version")
    if info.Version != 19 {
        return error("password: unsupported Argon2 version " + parts[2][2..])
    }
    var p Params
    p.Time = 0
    p.Memory = 0
    p.Threads = 0
    for kv in std.strings.Split(parts[3], ",") {
        var eq = std.strings.Index(kv, "=")
        if eq < 0 {
            return error("password: malformed parameters")
        }
        var v = std.strconv.Atoi(kv[eq + 1..])?("password: parameter " + kv[..eq])
        var k = kv[..eq]
        if k == "m" {
            p.Memory = v
        } else if k == "t" {
            p.Time = v
        } else if k == "p" {
            p.Threads = v
        } else {
            return error("password: unknown parameter " + k)
        }
    }
    if p.Memory <= 0 || p.Time <= 0 || p.Threads <= 0 {
        return error("password: missing parameters")
    }
    info.Salt = unb64(parts[4])?("password: salt")
    info.Key = unb64(parts[5])?("password: hash")
    p.KeyLen = len(info.Key)
    p.SaltLen = len(info.Salt)
    info.Params = p
    return info
}

// Whether pw matches hash (false for a malformed hash).
func Verify(pw string, hash string) bool {
    var info = try Parse(hash) catch err {
        return false
    }
    var kp = std.crypto.kdf.Argon2Params{
        Variant: info.Variant,
        Time: info.Params.Time,
        Memory: info.Params.Memory,
        Threads: info.Params.Threads,
        KeyLen: len(info.Key),
    }
    var key = try std.crypto.kdf.Argon2(std.strings.Bytes(pw), info.Salt, kp) catch err {
        return false
    }
    return std.crypto.hmac.Equal(key, info.Key)
}

// Whether hash should be recomputed with the default parameters (on the
// next successful login).
func NeedsRehash(hash string) bool {
    return NeedsRehashWith(hash, DefaultParams())
}

func NeedsRehashWith(hash string, p Params) bool {
    var info = try Parse(hash) catch err {
        return true
    }
    var q = info.Params
    return info.Variant != "argon2id" ||
        q.Time != p.Time ||
        q.Memory != p.Memory ||
        q.Threads != p.Threads ||
        q.KeyLen != p.KeyLen ||
        q.SaltLen < p.SaltLen
}
