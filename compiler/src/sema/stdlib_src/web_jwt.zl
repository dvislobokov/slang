// JSON Web Tokens (RFC 7519) with JWS compact serialization (RFC 7515):
// HS256 (HMAC-SHA256), RS256 (RSASSA-PKCS1-v1_5 with SHA-256) and ES256
// (ECDSA P-256 with SHA-256, raw r||s signatures) on std.crypto. Verify
// checks the algorithm against the key (no "none", no alg confusion), the
// signature, exp/nbf (with leeway), and iss/aud when asked to.
namespace std.web.auth.jwt

// A signing or verification key (HS256, RS256, ES256, RS256Public,
// ES256Public).
struct Key {
    Alg string
    // Optional key id ("kid" header).
    Kid string
    secret []byte
    rsaKey []byte
    rsaN []byte
    rsaE []byte
    ecPriv []byte
    ecPub []byte

    // Whether the key can sign (public keys only verify).
    func CanSign() bool {
        return len(this.secret) > 0 || len(this.rsaKey) > 0 || len(this.ecPriv) > 0
    }

    func WithKid(kid string) Key {
        var k = *this
        k.Kid = kid
        return k
    }
}

// An HMAC-SHA256 key (the shared secret signs and verifies).
func HS256(secret string) Key {
    return Key{Alg: "HS256", secret: std.strings.Bytes(secret)}
}

// An RSA key from its PKCS#1 private key DER (std.crypto.rsa.GenerateKey,
// x509.PrivateKey.Bytes): signs and verifies.
func RS256(privateKeyDER []byte) Key! {
    var pub = std.crypto.rsa.PublicKeyOf(privateKeyDER)?
    return Key{Alg: "RS256", rsaKey: privateKeyDER, rsaN: pub.N, rsaE: pub.E}
}

// An RSA public key (modulus and exponent, big-endian): verifies only.
func RS256Public(n []byte, e []byte) Key {
    return Key{Alg: "RS256", rsaN: n, rsaE: e}
}

// A P-256 key from its 32-byte private scalar (std.crypto.ecdsa.GenerateKey):
// signs and verifies.
func ES256(privateKey []byte) Key! {
    var pub = std.crypto.ecdsa.PublicKey(privateKey)?
    return Key{Alg: "ES256", ecPriv: privateKey, ecPub: pub}
}

// A P-256 public key (65-byte uncompressed point): verifies only.
func ES256Public(publicKey []byte) Key {
    return Key{Alg: "ES256", ecPub: publicKey}
}

// The registered claims and the common ones (name, email, roles); any
// other claim is in Extra (json values, also signed by Sign).
struct Claims {
    Subject string
    Issuer string
    Audience string
    // Unix seconds (0: absent).
    ExpiresAt int
    NotBefore int
    IssuedAt int
    Id string
    Name string
    Email string
    Roles []string
    Extra map[string]std.encoding.json.Value

    // A custom claim (null if absent).
    func Get(name string) std.encoding.json.Value {
        if this.Extra == nil {
            return std.encoding.json.Value{}
        }
        return this.Extra[name]
    }

    // A custom claim as a string ("" if absent or not a string).
    func String(name string) string {
        return try this.Get(name).String() else ""
    }

    // A custom claim holding a string or an array of strings.
    func Strings(name string) []string {
        var out []string = []
        var v = this.Get(name)
        if v.IsString() {
            out = append(out, try v.String() else "")
        } else if v.IsArray() {
            for x in v.Items() {
                if x.IsString() {
                    out = append(out, try x.String() else "")
                }
            }
        }
        return out
    }

    // Sets a custom claim.
    func Set(name string, v std.encoding.json.Value) {
        if this.Extra == nil {
            this.Extra = map[string]std.encoding.json.Value{}
        }
        this.Extra[name] = v
    }

    func HasRole(role string) bool {
        for r in this.Roles {
            if r == role {
                return true
            }
        }
        return false
    }

    // The payload JSON (registered claims first, then Extra by name).
    func Json() string {
        var b = std.strings.Builder{}
        b.Write("{")
        var n = 0
        n = member(&b, n, "sub", this.Subject)
        n = member(&b, n, "iss", this.Issuer)
        n = member(&b, n, "aud", this.Audience)
        n = numMember(&b, n, "exp", this.ExpiresAt)
        n = numMember(&b, n, "nbf", this.NotBefore)
        n = numMember(&b, n, "iat", this.IssuedAt)
        n = member(&b, n, "jti", this.Id)
        n = member(&b, n, "name", this.Name)
        n = member(&b, n, "email", this.Email)
        if len(this.Roles) > 0 {
            if n > 0 {
                b.Write(",")
            }
            b.Write("\"roles\":[")
            for i, r in this.Roles {
                if i > 0 {
                    b.Write(",")
                }
                b.Write(std.encoding.json.Quote(r))
            }
            b.Write("]")
            n += 1
        }
        if this.Extra != nil {
            var keys []string = []
            for k in this.Extra {
                keys = append(keys, k)
            }
            std.slices.Sort(keys)
            for k in keys {
                if n > 0 {
                    b.Write(",")
                }
                b.Write(std.encoding.json.Quote(k))
                b.Write(":")
                b.Write(this.Extra[k].Format())
                n += 1
            }
        }
        b.Write("}")
        return b.String()
    }
}

func member(b *std.strings.Builder, n int, name string, v string) int {
    if v == "" {
        return n
    }
    if n > 0 {
        b.Write(",")
    }
    b.Write(std.encoding.json.Quote(name))
    b.Write(":")
    b.Write(std.encoding.json.Quote(v))
    return n + 1
}

func numMember(b *std.strings.Builder, n int, name string, v int) int {
    if v == 0 {
        return n
    }
    if n > 0 {
        b.Write(",")
    }
    b.Write(std.encoding.json.Quote(name))
    b.Write(":")
    b.Write(std.fmt.Format("{0}", v))
    return n + 1
}

// Options of Verify.
struct VerifyOptions {
    // Required "iss" / "aud" values ("" = not checked).
    Issuer string
    Audience string
    // Seconds of clock skew allowed for exp and nbf.
    Leeway int
    // Unix-seconds clock (nil: the system clock).
    Clock func() int
}

func b64(s string) string {
    return std.encoding.base64.EncodeURL(std.strings.Bytes(s))
}

// Signs claims with k: header.payload.signature.
func Sign(claims Claims, k Key) string! {
    if !k.CanSign() {
        return error("jwt: the " + k.Alg + " key cannot sign (a public key)")
    }
    var header = "{\"alg\":" + std.encoding.json.Quote(k.Alg) + ",\"typ\":\"JWT\""
    if k.Kid != "" {
        header += ",\"kid\":" + std.encoding.json.Quote(k.Kid)
    }
    header += "}"
    var input = b64(header) + "." + b64(claims.Json())
    var sig = signature(input, k)?
    return input + "." + std.encoding.base64.EncodeURL(sig)
}

func signature(input string, k Key) []byte! {
    if k.Alg == "HS256" {
        return std.crypto.hmac.Sha256(std.strings.FromBytes(k.secret), input)
    }
    var digest = std.crypto.hash.Sha256(input)
    if k.Alg == "RS256" {
        return std.crypto.rsa.SignPKCS1v15(k.rsaKey, "sha256", digest)?
    }
    if k.Alg == "ES256" {
        var der = std.crypto.ecdsa.Sign(k.ecPriv, digest)?
        return derToRaw(der)?
    }
    return error("jwt: unsupported algorithm " + k.Alg)
}

// The parts of a token without verifying it (header JSON and claims).
func Decode(token string) (std.encoding.json.Value, Claims)! {
    var parts = std.strings.Split(token, ".")
    if len(parts) != 3 {
        return error("jwt: malformed token (expected header.payload.signature)")
    }
    var hb = try std.encoding.base64.DecodeURL(parts[0]) catch err {
        return error("jwt: malformed header")
    }
    var header = try std.encoding.json.ParseValue(std.strings.FromBytes(hb)) catch err {
        return error("jwt: malformed header")
    }
    var pb = try std.encoding.base64.DecodeURL(parts[1]) catch err {
        return error("jwt: malformed payload")
    }
    var payload = try std.encoding.json.ParseValue(std.strings.FromBytes(pb)) catch err {
        return error("jwt: malformed payload")
    }
    if !payload.IsObject() {
        return error("jwt: the payload is not a JSON object")
    }
    return (header, claimsOf(payload))
}

func claimsOf(v std.encoding.json.Value) Claims {
    var c = Claims{}
    for k in v.Keys() {
        var x = v.Get(k)
        if k == "sub" {
            c.Subject = try x.String() else ""
        } else if k == "iss" {
            c.Issuer = try x.String() else ""
        } else if k == "aud" {
            if x.IsArray() && x.Len() > 0 {
                c.Audience = try x.Index(0).String() else ""
            } else {
                c.Audience = try x.String() else ""
            }
        } else if k == "exp" {
            c.ExpiresAt = try x.Int() else 0
        } else if k == "nbf" {
            c.NotBefore = try x.Int() else 0
        } else if k == "iat" {
            c.IssuedAt = try x.Int() else 0
        } else if k == "jti" {
            c.Id = try x.String() else ""
        } else if k == "name" {
            c.Name = try x.String() else ""
        } else if k == "email" {
            c.Email = try x.String() else ""
        } else if k == "roles" || k == "role" {
            var rs []string = []
            if x.IsArray() {
                for r in x.Items() {
                    rs = append(rs, try r.String() else "")
                }
            } else if x.IsString() {
                rs = append(rs, try x.String() else "")
            }
            c.Roles = rs
        } else {
            c.Set(k, x)
        }
    }
    return c
}

// Verifies token with k and returns its claims.
func Verify(token string, k Key, o VerifyOptions) Claims! {
    var (header, claims) = Decode(token)?
    var alg = try header.Get("alg").String() else ""
    if alg != k.Alg {
        return error("jwt: the token is signed with " + std.encoding.json.Quote(alg) + ", the key is " + k.Alg)
    }
    var dot = std.strings.LastIndex(token, ".")
    var input = token[..dot]
    var sig = try std.encoding.base64.DecodeURL(token[dot + 1..]) catch err {
        return error("jwt: malformed signature")
    }
    if !verifySignature(input, sig, k) {
        return error("jwt: invalid signature")
    }
    var now = 0
    if o.Clock != nil {
        now = o.Clock()
    } else {
        now = std.time.Now() / 1000
    }
    if claims.ExpiresAt != 0 && now > claims.ExpiresAt + o.Leeway {
        return error("jwt: the token has expired")
    }
    if claims.NotBefore != 0 && now + o.Leeway < claims.NotBefore {
        return error("jwt: the token is not valid yet")
    }
    if o.Issuer != "" && claims.Issuer != o.Issuer {
        return error("jwt: unexpected issuer " + std.encoding.json.Quote(claims.Issuer))
    }
    if o.Audience != "" && claims.Audience != o.Audience {
        return error("jwt: unexpected audience " + std.encoding.json.Quote(claims.Audience))
    }
    return claims
}

func verifySignature(input string, sig []byte, k Key) bool {
    if k.Alg == "HS256" {
        if len(k.secret) == 0 {
            return false
        }
        return std.crypto.hmac.Equal(std.crypto.hmac.Sha256(std.strings.FromBytes(k.secret), input), sig)
    }
    var digest = std.crypto.hash.Sha256(input)
    if k.Alg == "RS256" {
        return len(k.rsaN) > 0 && std.crypto.rsa.VerifyPKCS1v15(k.rsaN, k.rsaE, "sha256", digest, sig)
    }
    if k.Alg == "ES256" {
        if len(sig) != 64 || len(k.ecPub) == 0 {
            return false
        }
        return std.crypto.ecdsa.Verify(k.ecPub, digest, rawToDer(sig))
    }
    return false
}

// ---- ECDSA signature formats -------------------------------------------------------

// DER ECDSA-Sig-Value -> 64 bytes r||s.
func derToRaw(der []byte) []byte! {
    if len(der) < 8 || der[0] != 48 {
        return error("jwt: malformed ECDSA signature")
    }
    var pos = 2
    if der[1] > 128 {
        pos = 2 + int(der[1] - 128)
    }
    var out []byte = []
    for _ in 0..2 {
        if pos + 2 > len(der) || der[pos] != 2 {
            return error("jwt: malformed ECDSA signature")
        }
        var n = int(der[pos + 1])
        var v = der[pos + 2..pos + 2 + n]
        for len(v) > 32 && v[0] == 0 {
            v = v[1..]
        }
        if len(v) > 32 {
            return error("jwt: malformed ECDSA signature")
        }
        for _ in 0..32 - len(v) {
            out = append(out, byte(0))
        }
        for b in v {
            out = append(out, b)
        }
        pos = pos + 2 + n
    }
    return out
}

func derInt(v []byte) []byte {
    var x = v
    for len(x) > 1 && x[0] == 0 {
        x = x[1..]
    }
    var out []byte = [byte(2)]
    if x[0] >= 128 {
        out = append(out, byte(len(x) + 1))
        out = append(out, byte(0))
    } else {
        out = append(out, byte(len(x)))
    }
    for b in x {
        out = append(out, b)
    }
    return out
}

// 64 bytes r||s -> DER ECDSA-Sig-Value.
func rawToDer(raw []byte) []byte {
    var r = derInt(raw[..32])
    var s = derInt(raw[32..])
    var out []byte = [byte(48), byte(len(r) + len(s))]
    for b in r {
        out = append(out, b)
    }
    for b in s {
        out = append(out, b)
    }
    return out
}
