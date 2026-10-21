// The Slang-source std.net.tls (see sema/stdsrc.rs for the rules): TLS 1.3
// (RFC 8446) and TLS 1.2 restricted to ECDHE + AEAD (RFC 5246, 5288, 7905,
// 7627), client and server, over std.net TCP connections. This file holds
// the public API, the key schedule and the record layer; the handshake
// state machines are in tls_client.sl and tls_server.sl. Cryptography is
// done by the C primitives of std.crypto.* (runtime/src/crypto/); the
// record AEAD contexts live in []byte blobs (rtAeadNew).
//
// Wire constants are written as numbers with their RFC names in comments:
// record types 20 change_cipher_spec, 21 alert, 22 handshake,
// 23 application_data; handshake types 1 client_hello, 2 server_hello,
// 4 new_session_ticket, 8 encrypted_extensions, 11 certificate,
// 12 server_key_exchange, 13 certificate_request, 14 server_hello_done,
// 15 certificate_verify, 16 client_key_exchange, 20 finished,
// 24 key_update, 254 message_hash.
namespace std.net.tls

// ---- byte helpers -------------------------------------------------------------

func u16(b []byte, i int) int {
    return int(b[i]) * 256 + int(b[i + 1])
}

func u24(b []byte, i int) int {
    return (int(b[i]) * 256 + int(b[i + 1])) * 256 + int(b[i + 2])
}

func bytesOf(s string) []byte {
    return std.strings.Bytes(s)
}

func cat(a []byte, b []byte) []byte {
    return rtConcat(a, b)
}

func cat3(a []byte, b []byte, c []byte) []byte {
    return rtConcat(rtConcat(a, b), c)
}

func same(a []byte, b []byte) bool {
    if len(a) != len(b) {
        return false
    }
    for i in 0..len(a) {
        if a[i] != b[i] {
            return false
        }
    }
    return true
}

func randomBytes(n int) []byte! {
    return std.crypto.rand.Bytes(n)?
}

// Big-endian encoding of v in k bytes.
func beBytes(v int, k int) []byte {
    var out = rtZeros(k)
    var x = v
    var i = k - 1
    for i >= 0 {
        out[i] = byte(x & 255)
        x = x >> 8
        i -= 1
    }
    return out
}

// Builder of wire messages; open/close patch length prefixes.
struct builder {
    b []byte

    func u8(v int) {
        this.b = append(this.b, byte(v & 255))
    }

    func u16(v int) {
        this.b = append(this.b, byte((v >> 8) & 255))
        this.b = append(this.b, byte(v & 255))
    }

    func u24(v int) {
        this.u8(v >> 16)
        this.u16(v & 65535)
    }

    func u32(v int) {
        this.u16((v >> 16) & 65535)
        this.u16(v & 65535)
    }

    func raw(x []byte) {
        if len(x) > 0 {
            this.b = rtConcat(this.b, x)
        }
    }

    // Reserves a k-byte length prefix; returns its position.
    func open(k int) int {
        var pos = len(this.b)
        for len(this.b) < pos + k {
            this.b = append(this.b, byte(0))
        }
        return pos
    }

    func close(pos int, k int) {
        patchLen(this.b, pos, k, len(this.b) - pos - k)
    }

    func vec8(x []byte) {
        this.u8(len(x))
        this.raw(x)
    }

    func vec16(x []byte) {
        this.u16(len(x))
        this.raw(x)
    }

    func vec24(x []byte) {
        this.u24(len(x))
        this.raw(x)
    }

    func bytes() []byte {
        return this.b
    }
}

func patchLen(b []byte, pos int, k int, n int) {
    var x = n
    var i = k - 1
    for i >= 0 {
        b[pos + i] = byte(x & 255)
        x = x >> 8
        i -= 1
    }
}

func newBuilder() *builder {
    var b []byte = []
    return new builder{b: b}
}

// Reader of wire messages with a sticky failure flag (check ok() once).
struct parser {
    b   []byte
    off int
    bad bool

    func left() int {
        return len(this.b) - this.off
    }

    func empty() bool {
        return this.off >= len(this.b)
    }

    func ok() bool {
        return !this.bad
    }

    func u8() int {
        if this.left() < 1 {
            this.bad = true
            return 0
        }
        var v = int(this.b[this.off])
        this.off += 1
        return v
    }

    func u16() int {
        if this.left() < 2 {
            this.bad = true
            this.off = len(this.b)
            return 0
        }
        var v = u16(this.b, this.off)
        this.off += 2
        return v
    }

    func u24() int {
        if this.left() < 3 {
            this.bad = true
            this.off = len(this.b)
            return 0
        }
        var v = u24(this.b, this.off)
        this.off += 3
        return v
    }

    func u32() int {
        var hi = this.u16()
        return hi * 65536 + this.u16()
    }

    func take(n int) []byte {
        if n < 0 || this.left() < n {
            this.bad = true
            this.off = len(this.b)
            var none []byte = []
            return none
        }
        var v = this.b[this.off..this.off + n]
        this.off += n
        return v
    }

    func vec8() []byte {
        return this.take(this.u8())
    }

    func vec16() []byte {
        return this.take(this.u16())
    }

    func vec24() []byte {
        return this.take(this.u24())
    }

    func rest() []byte {
        return this.take(this.left())
    }
}

func newParser(b []byte) *parser {
    return new parser{b: b}
}

// ---- errors and alerts --------------------------------------------------------

func alertText(d int) string {
    switch d {
        case 0: return "close notify"
        case 10: return "unexpected message"
        case 20: return "bad record MAC"
        case 22: return "record overflow"
        case 40: return "handshake failure"
        case 42: return "bad certificate"
        case 43: return "unsupported certificate"
        case 44: return "revoked certificate"
        case 45: return "expired certificate"
        case 46: return "unknown certificate"
        case 47: return "illegal parameter"
        case 48: return "unknown certificate authority"
        case 49: return "access denied"
        case 50: return "error decoding message"
        case 51: return "error decrypting message"
        case 70: return "protocol version not supported"
        case 71: return "insufficient security level"
        case 80: return "internal error"
        case 86: return "inappropriate fallback"
        case 90: return "user canceled"
        case 100: return "no renegotiation"
        case 109: return "missing extension"
        case 110: return "unsupported extension"
        case 112: return "unrecognized name"
        case 116: return "certificate required"
        case 120: return "no application protocol"
    }
    return std.fmt.Format("alert({0})", d)
}

// ---- cipher suites, groups and signature schemes ------------------------------

// What a cipher suite needs.
struct suiteInfo {
    id     int
    name   string
    hash   string
    keyLen int
    // 1 AES-GCM, 2 ChaCha20-Poly1305
    aead int
    v13  bool
    // TLS 1.2 authentication: "ECDSA" or "RSA"
    auth string
}

func suiteByID(id int) suiteInfo {
    switch id {
        case 4865:
            return suiteInfo{
                id: id,
                name: "TLS_AES_128_GCM_SHA256",
                hash: "sha256",
                keyLen: 16,
                aead: 1,
                v13: true,
            }
        case 4866:
            return suiteInfo{
                id: id,
                name: "TLS_AES_256_GCM_SHA384",
                hash: "sha384",
                keyLen: 32,
                aead: 1,
                v13: true,
            }
        case 4867:
            return suiteInfo{
                id: id,
                name: "TLS_CHACHA20_POLY1305_SHA256",
                hash: "sha256",
                keyLen: 32,
                aead: 2,
                v13: true,
            }
        case 49195:
            return suiteInfo{
                id: id,
                name: "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",
                hash: "sha256",
                keyLen: 16,
                aead: 1,
                auth: "ECDSA",
            }
        case 49196:
            return suiteInfo{
                id: id,
                name: "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384",
                hash: "sha384",
                keyLen: 32,
                aead: 1,
                auth: "ECDSA",
            }
        case 49199:
            return suiteInfo{
                id: id,
                name: "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
                hash: "sha256",
                keyLen: 16,
                aead: 1,
                auth: "RSA",
            }
        case 49200:
            return suiteInfo{
                id: id,
                name: "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384",
                hash: "sha384",
                keyLen: 32,
                aead: 1,
                auth: "RSA",
            }
        case 52393:
            return suiteInfo{
                id: id,
                name: "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256",
                hash: "sha256",
                keyLen: 32,
                aead: 2,
                auth: "ECDSA",
            }
        case 52392:
            return suiteInfo{
                id: id,
                name: "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256",
                hash: "sha256",
                keyLen: 32,
                aead: 2,
                auth: "RSA",
            }
    }
    return suiteInfo{id: 0}
}

// CipherSuiteName is the IANA name of a supported suite ("" if unknown).
func CipherSuiteName(id int) string {
    return suiteByID(id).name
}

// Suites in preference order (ChaCha20 first without AES hardware).
func suites13() []int {
    if std.crypto.aes.HasHardware() {
        return [4865, 4866, 4867]
    }
    return [4867, 4865, 4866]
}

func suites12() []int {
    if std.crypto.aes.HasHardware() {
        return [49195, 49199, 49196, 49200, 52393, 52392]
    }
    return [52393, 52392, 49195, 49199, 49196, 49200]
}

func hashLen(h string) int {
    if h == "sha384" {
        return 48
    }
    if h == "sha512" {
        return 64
    }
    return 32
}

func hashOf(h string, data []byte) []byte! {
    return std.crypto.hash.Sum(h, data)?
}

func containsInt(xs []int, v int) bool {
    for x in xs {
        if x == v {
            return true
        }
    }
    return false
}

func versionName(v int) string {
    if v == 772 {
        return "TLS 1.3"
    }
    if v == 771 {
        return "TLS 1.2"
    }
    return std.fmt.Sprintf("0x%04x", v)
}

// Signature schemes we can verify, in our preference order:
// ecdsa_secp256r1_sha256, ecdsa_secp384r1_sha384, ed25519,
// rsa_pss_rsae_sha256/384/512, rsa_pkcs1_sha256/384/512 (TLS 1.2 and
// certificates only).
func ourSchemes() []int {
    return [1027, 1283, 2055, 2052, 2053, 2054, 1025, 1281, 1537]
}

func schemeHash(s int) string {
    switch s {
        case 1027, 2052, 1025: return "sha256"
        case 2053, 1281, 1283: return "sha384"
        case 2054, 1537: return "sha512"
    }
    return ""
}

// The scheme to sign with for key, given what the peer accepts.
func pickScheme(key std.crypto.x509.PrivateKey, peer []int, v13 bool) int {
    var prefs []int = []
    if key.Algorithm == "ECDSA" && len(key.Bytes) == 48 {
        prefs = [1283]
    } else if key.Algorithm == "ECDSA" {
        prefs = [1027]
    } else if key.Algorithm == "Ed25519" {
        prefs = [2055]
    } else if key.Algorithm == "RSA" {
        if v13 {
            prefs = [2052, 2053, 2054]
        } else {
            prefs = [2052, 2053, 2054, 1025, 1281, 1537]
        }
    }
    for p in prefs {
        if containsInt(peer, p) {
            return p
        }
    }
    return 0
}

func signWith(key std.crypto.x509.PrivateKey, scheme int, content []byte) []byte! {
    if scheme == 2055 {
        return std.crypto.ed25519.Sign(key.Bytes, content)?
    }
    var h = schemeHash(scheme)
    var digest = hashOf(h, content)?
    if scheme == 1027 {
        return std.crypto.ecdsa.Sign(key.Bytes, digest)?
    }
    if scheme == 1283 {
        return std.crypto.ecdsa.SignP384(key.Bytes, digest)?
    }
    if scheme >= 2052 && scheme <= 2054 {
        return std.crypto.rsa.SignPSS(key.Bytes, h, digest)?
    }
    return std.crypto.rsa.SignPKCS1v15(key.Bytes, h, digest)?
}

func verifyWith(
    cert *std.crypto.x509.Certificate,
    scheme int,
    content []byte,
    sig []byte,
    v13 bool,
) void! {
    var alg = cert.PublicKeyAlgorithm
    if scheme == 2055 {
        if alg != "Ed25519" || !std.crypto.ed25519.Verify(cert.PublicKey, content, sig) {
            return error("tls: invalid Ed25519 signature from the peer")
        }
        return
    }
    var h = schemeHash(scheme)
    if h == "" || (v13 && scheme >= 1025 && scheme <= 1537 && scheme % 256 == 1) {
        return error(
            std.fmt.Sprintf("tls: peer used an unsupported signature scheme 0x%04x", scheme),
        )
    }
    var digest = hashOf(h, content)?
    if scheme == 1027 || scheme == 1283 {
        // TLS 1.3 ties the curve to the scheme; TLS 1.2 only the hash.
        var curve = cert.Curve
        if v13 && ((scheme == 1027 && curve != "P-256") || (scheme == 1283 && curve != "P-384")) {
            return error("tls: ECDSA signature scheme does not match the certificate curve")
        }
        var good = false
        if alg == "ECDSA" && curve == "P-256" {
            good = std.crypto.ecdsa.Verify(cert.PublicKey, digest, sig)
        } else if alg == "ECDSA" && curve == "P-384" {
            good = std.crypto.ecdsa.VerifyP384(cert.PublicKey, digest, sig)
        }
        if !good {
            return error("tls: invalid ECDSA signature from the peer")
        }
        return
    }
    if alg != "RSA" {
        return error("tls: RSA signature scheme with a " + alg + " certificate")
    }
    var good = false
    if scheme >= 2052 && scheme <= 2054 {
        good = std.crypto.rsa.VerifyPSS(cert.PublicKey, cert.RSAExponent, h, digest, sig)
    } else {
        good = std.crypto.rsa.VerifyPKCS1v15(cert.PublicKey, cert.RSAExponent, h, digest, sig)
    }
    if !good {
        return error("tls: invalid RSA signature from the peer")
    }
}

// An ephemeral key exchange share: group 29 x25519, 23 secp256r1 or 24
// secp384r1.
struct keyShare {
    group int
    priv  []byte
    pub   []byte
}

func newKeyShare(group int) keyShare! {
    if group == 29 {
        var p = std.crypto.x25519.GenerateKey()?
        return keyShare{group: 29, priv: p, pub: std.crypto.x25519.PublicKey(p)?}
    }
    if group == 24 {
        var p = std.crypto.ecdh.GenerateKeyP384()?
        return keyShare{group: 24, priv: p, pub: std.crypto.ecdh.PublicKeyP384(p)?}
    }
    var p = std.crypto.ecdh.GenerateKey()?
    return keyShare{group: 23, priv: p, pub: std.crypto.ecdh.PublicKey(p)?}
}

func sharedSecret(ks keyShare, peer []byte) []byte! {
    if ks.group == 29 {
        if len(peer) != 32 {
            return error("tls: invalid X25519 key share")
        }
        return std.crypto.x25519.SharedSecret(ks.priv, peer)?
    }
    if ks.group == 24 {
        if len(peer) != 97 {
            return error("tls: invalid P-384 key share")
        }
        return std.crypto.ecdh.SharedSecretP384(ks.priv, peer)?
    }
    if len(peer) != 65 {
        return error("tls: invalid P-256 key share")
    }
    return std.crypto.ecdh.SharedSecret(ks.priv, peer)?
}

// ---- key schedule ---------------------------------------------------------------

// ExpandLabel is HKDF-Expand-Label of TLS 1.3 (RFC 8446 7.1): the hash is
// "sha256" or "sha384", label without the "tls13 " prefix.
func ExpandLabel(hash string, secret []byte, label string, context []byte, length int) []byte! {
    var b = newBuilder()
    b.u16(length)
    b.vec8(bytesOf("tls13 " + label))
    b.vec8(context)
    return std.crypto.hkdf.Expand(hash, secret, b.bytes(), length)?
}

// DeriveSecret is Derive-Secret(secret, label, messages) of RFC 8446 7.1
// taking the transcript hash of the messages.
func DeriveSecret(hash string, secret []byte, label string, transcriptHash []byte) []byte! {
    return ExpandLabel(hash, secret, label, transcriptHash, hashLen(hash))?
}

// The early secret (psk nil: no PSK).
func earlySecret(h string, psk []byte) []byte! {
    var ikm = psk
    if len(ikm) == 0 {
        ikm = rtZeros(hashLen(h))
    }
    return std.crypto.hkdf.Extract(h, [], ikm)?
}

// Next extract stage: HKDF-Extract(Derive-Secret(prev, "derived", ""), ikm).
func nextSecret(h string, prev []byte, ikm []byte) []byte! {
    var empty = hashOf(h, [])?
    var salt = DeriveSecret(h, prev, "derived", empty)?
    var input = ikm
    if len(input) == 0 {
        input = rtZeros(hashLen(h))
    }
    return std.crypto.hkdf.Extract(h, salt, input)?
}

func finishedMAC(h string, baseKey []byte, transcriptHash []byte) []byte! {
    var fk = ExpandLabel(h, baseKey, "finished", [], hashLen(h))?
    return std.crypto.hmac.Sum(h, fk, transcriptHash)?
}

// TLS 1.2 PRF (RFC 5246 5): P_hash(secret, label + seed).
func prf12(h string, secret []byte, label string, seed []byte, n int) []byte! {
    var ls = cat(bytesOf(label), seed)
    var a = ls
    var out []byte = []
    for len(out) < n {
        a = std.crypto.hmac.Sum(h, secret, a)?
        out = cat(out, std.crypto.hmac.Sum(h, secret, cat(a, ls))?)
    }
    return out[..n]
}

// ---- record protection -------------------------------------------------------------

// The protection of one direction.
struct cipherState {
    // AEAD context (empty: plaintext records)
    ctx []byte
    // 12-byte IV (TLS 1.3, ChaCha20 in 1.2) or 4-byte salt (AES-GCM in 1.2)
    iv  []byte
    seq int
    v13 bool
    // TLS 1.3 traffic secret (for KeyUpdate) and its hash
    secret []byte
    suite  suiteInfo

    func active() bool {
        return len(this.ctx) > 0
    }
}

func cipher13(s suiteInfo, secret []byte) cipherState! {
    var key = ExpandLabel(s.hash, secret, "key", [], s.keyLen)?
    var iv = ExpandLabel(s.hash, secret, "iv", [], 12)?
    return cipherState{ctx: rtAeadNew(s.aead, key)?, iv: iv, v13: true, secret: secret, suite: s}
}

func cipher12(s suiteInfo, key []byte, iv []byte) cipherState! {
    return cipherState{ctx: rtAeadNew(s.aead, key)?, iv: iv, suite: s}
}

// iv XOR the 64-bit sequence number (right-aligned).
func xorNonce(iv []byte, seq int) []byte {
    var n = cat(iv, [])
    var s = beBytes(seq, 8)
    for i in 0..8 {
        n[4 + i] = n[4 + i] ^ s[i]
    }
    return n
}

// Protects one record: returns header + body.
func sealRecord(cs *cipherState, typ int, data []byte) []byte {
    var hdr = newBuilder()
    if !cs.active() {
        hdr.u8(typ)
        hdr.u16(771)
        hdr.u16(len(data))
        return cat(hdr.bytes(), data)
    }
    var out []byte = []
    if cs.v13 {
        // TLSInnerPlaintext = content || type; outer type application_data
        var inner = cat(data, [byte(typ)])
        hdr.u8(23)
        hdr.u16(771)
        hdr.u16(len(inner) + 16)
        var aad = hdr.bytes()
        out = cat(aad, rtAeadSeal(cs.ctx, xorNonce(cs.iv, cs.seq), aad, inner))
    } else {
        var ad = newBuilder()
        ad.raw(beBytes(cs.seq, 8))
        ad.u8(typ)
        ad.u16(771)
        ad.u16(len(data))
        if cs.suite.aead == 1 {
            var explicit = beBytes(cs.seq, 8)
            var sealed = rtAeadSeal(cs.ctx, cat(cs.iv, explicit), ad.bytes(), data)
            hdr.u8(typ)
            hdr.u16(771)
            hdr.u16(8 + len(sealed))
            out = cat3(hdr.bytes(), explicit, sealed)
        } else {
            var sealed = rtAeadSeal(cs.ctx, xorNonce(cs.iv, cs.seq), ad.bytes(), data)
            hdr.u8(typ)
            hdr.u16(771)
            hdr.u16(len(sealed))
            out = cat(hdr.bytes(), sealed)
        }
    }
    cs.seq += 1
    return out
}

// A record after removing the protection.
struct record {
    typ  int
    data []byte
}

func openRecord(cs *cipherState, hdr []byte, body []byte) record! {
    var typ = int(hdr[0])
    if !cs.active() {
        return record{typ: typ, data: body}
    }
    if cs.v13 {
        if typ == 20 {
            // compatibility change_cipher_spec is never protected
            return record{typ: typ, data: body}
        }
        if typ != 23 {
            return error("tls: unexpected unprotected record")
        }
        var inner = rtAeadOpen(cs.ctx, xorNonce(cs.iv, cs.seq), hdr, body)?
        cs.seq += 1
        var i = len(inner) - 1
        for i >= 0 && int(inner[i]) == 0 {
            i -= 1
        }
        if i < 0 {
            return error("tls: record without content type")
        }
        return record{typ: int(inner[i]), data: inner[..i]}
    }
    if typ == 20 {
        return error("tls: unexpected change_cipher_spec")
    }
    var ad = newBuilder()
    ad.raw(beBytes(cs.seq, 8))
    ad.u8(typ)
    ad.u16(771)
    var plain []byte = []
    if cs.suite.aead == 1 {
        if len(body) < 24 {
            return error("tls: bad record MAC")
        }
        ad.u16(len(body) - 24)
        plain = rtAeadOpen(cs.ctx, cat(cs.iv, body[..8]), ad.bytes(), body[8..])?
    } else {
        if len(body) < 16 {
            return error("tls: bad record MAC")
        }
        ad.u16(len(body) - 16)
        plain = rtAeadOpen(cs.ctx, xorNonce(cs.iv, cs.seq), ad.bytes(), body)?
    }
    cs.seq += 1
    return record{typ: typ, data: plain}
}

// SealRecord protects one TLS 1.3 record (RFC 8446 5.2) with the traffic
// key and IV of a suite (0x1301..0x1303) at sequence number seq, and
// returns the whole record. Low-level: for tests and tools.
func SealRecord(suite int, key []byte, iv []byte, seq int, contentType int, data []byte) []byte! {
    var s = suiteByID(suite)
    if !s.v13 {
        return error("tls: SealRecord needs a TLS 1.3 cipher suite")
    }
    var cs = new cipherState{ctx: rtAeadNew(s.aead, key)?, iv: iv, seq: seq, v13: true, suite: s}
    return sealRecord(cs, contentType, data)
}

// OpenRecord removes the protection of a TLS 1.3 record made by SealRecord
// and returns the inner content type followed by the content.
func OpenRecord(suite int, key []byte, iv []byte, seq int, rec []byte) []byte! {
    var s = suiteByID(suite)
    if !s.v13 || len(rec) < 5 || int(rec[0]) != 23 || u16(rec, 3) != len(rec) - 5 {
        return error("tls: OpenRecord needs a TLS 1.3 cipher suite and a whole record")
    }
    var cs = new cipherState{ctx: rtAeadNew(s.aead, key)?, iv: iv, seq: seq, v13: true, suite: s}
    var r = openRecord(cs, rec[..5], rec[5..])?
    return cat([byte(r.typ)], r.data)
}

// ---- configuration --------------------------------------------------------------------

// Certificate is a certificate chain (leaf first) with its private key.
struct Certificate {
    Chain []*std.crypto.x509.Certificate
    Key   std.crypto.x509.PrivateKey
}

// LoadKeyPair parses a PEM certificate chain and a PEM private key and
// checks that the key belongs to the leaf.
func LoadKeyPair(certPEM string, keyPEM string) Certificate! {
    var chain = std.crypto.x509.ParsePEM(certPEM)?
    var key = std.crypto.x509.ParsePrivateKeyPEM(keyPEM)?
    if !key.Matches(chain[0]) {
        return error("tls: private key does not match the certificate")
    }
    return Certificate{Chain: chain, Key: key}
}

// LoadKeyPairFiles is LoadKeyPair reading two files.
func LoadKeyPairFiles(certFile string, keyFile string) Certificate! {
    return LoadKeyPair(std.io.ReadFile(certFile)?, std.io.ReadFile(keyFile)?)?
}

// Config configures a client.
struct Config {
    // Host name for SNI and certificate verification (default: the host
    // part of the Dial address).
    ServerName string
    // Application protocols to offer, in preference order ("h2", "http/1.1").
    ALPN []string
    // Trust anchors; nil means the system roots (std.crypto.x509.SystemRoots).
    RootCAs *std.crypto.x509.CertPool
    // Certificate presented when the server asks for one.
    ClientCert *Certificate
    // Skips the chain and host name checks (the handshake signature is
    // still checked). Only for tests and self-signed development servers.
    InsecureSkipVerify bool
    // Oldest and newest versions (V12, V13); 0 means V12 and V13.
    MinVersion int
    MaxVersion int
    // Cache of session tickets for resumption (nil: no resumption).
    Sessions *SessionCache
}

// ServerConfig configures a server.
struct ServerConfig {
    // Certificates to serve; the first whose names match the SNI host is
    // used, else the first.
    Certificates []Certificate
    // NoClientCert (default), RequestClientCert, RequireAnyClientCert,
    // VerifyClientCertIfGiven or RequireAndVerifyClientCert.
    ClientAuth int
    // Roots for verifying client certificates (nil: system roots).
    ClientCAs  *std.crypto.x509.CertPool
    MinVersion int
    MaxVersion int
    // Application protocols the server supports, in preference order.
    ALPN []string
    // Turns off session tickets (TLS 1.3 PSK and RFC 5077 tickets).
    DisableSessionTickets bool
    // 32-byte key protecting tickets (random per Server when empty).
    TicketKey []byte
}

// ConnectionState describes an established connection.
struct ConnectionState {
    // V12 or V13.
    Version         int
    CipherSuite     int
    CipherSuiteName string
    // Negotiated application protocol ("" if none).
    ALPN       string
    ServerName string
    // Leaf first, as sent by the peer.
    PeerCertificates []*std.crypto.x509.Certificate
    // The verified chain (empty when not verified).
    VerifiedChain []*std.crypto.x509.Certificate
    // The handshake resumed an earlier session.
    Resumed           bool
    HandshakeComplete bool

    // VersionName returns the negotiated version as "TLS 1.3" or "TLS 1.2".
    func VersionName() string {
        return versionName(this.Version)
    }
}

// A resumable session as remembered by a client.
struct session {
    version int
    suite   int
    ticket  []byte
    // TLS 1.3: the PSK; TLS 1.2: the master secret
    secret    []byte
    ageAdd    int
    received  int
    lifetime  int
    alpn      string
    peerCerts []*std.crypto.x509.Certificate
    verified  []*std.crypto.x509.Certificate
}

// SessionCache keeps the latest session ticket per server for clients
// (Config.Sessions). Safe to share between connections.
struct SessionCache {
    guard int
    m     map[string]*session

    func get(key string) *session {
        std.task.rtLock(&this.guard)
        var s = this.m[key]
        std.task.rtUnlock(&this.guard)
        if s != nil && std.time.Now() > s.received + s.lifetime * 1000 {
            return nil
        }
        return s
    }

    func put(key string, s *session) {
        std.task.rtLock(&this.guard)
        this.m[key] = s
        std.task.rtUnlock(&this.guard)
    }

    func remove(key string) {
        std.task.rtLock(&this.guard)
        delete(this.m, key)
        std.task.rtUnlock(&this.guard)
    }

    // Number of cached sessions.
    func Len() int {
        std.task.rtLock(&this.guard)
        var n = len(this.m)
        std.task.rtUnlock(&this.guard)
        return n
    }
}

// NewSessionCache returns an empty client session cache (for resumption).
func NewSessionCache() *SessionCache {
    return new SessionCache{m: map[string]*session{}}
}

func minVersion(v int) int {
    if v <= 771 {
        return 771
    }
    return v
}

func maxVersion(v int) int {
    if v == 0 || v > 772 {
        return 772
    }
    return v
}

// ---- connections ------------------------------------------------------------------------

// Conn is a TLS connection. Copies share the connection. The handshake
// runs on the first Read or Write (or Handshake()).
struct Conn {
    c *conn

    // Runs the handshake if it has not run yet.
    func Handshake() Task<void!> {
        return this.c.handshake()
    }

    // Reads up to n bytes of application data; an empty result means the
    // peer closed the connection (close_notify or end of stream).
    func Read(n int) Task<[]byte!> {
        return this.c.read(n)
    }

    // ReadString is Read as a string.
    func ReadString(n int) Task<string!> {
        return this.c.readString(n)
    }

    // Writes all of data; yields the number of bytes written.
    func Write(data []byte) Task<int!> {
        return this.c.write(data)
    }

    // WriteString writes text.
    func WriteString(text string) Task<int!> {
        return this.c.write(bytesOf(text))
    }

    // Sends close_notify (best effort) and closes the socket.
    func Close() {
        this.c.close()
    }

    // IsClosed reports whether the connection is closed.
    func IsClosed() bool {
        return this.c.closed
    }

    // Timeout in ms for each later network read/write (0: none).
    func SetTimeout(ms int) {
        this.c.raw.SetTimeout(ms)
    }

    // LocalAddr returns the local "ip:port".
    func LocalAddr() string {
        return this.c.raw.LocalAddr()
    }

    // RemoteAddr returns the peer's "ip:port".
    func RemoteAddr() string {
        return this.c.raw.RemoteAddr()
    }

    // ConnectionState returns the negotiated parameters of the handshake.
    func ConnectionState() ConnectionState {
        return this.c.state()
    }

    // Updates the sending keys and asks the peer to update its own
    // (TLS 1.3 KeyUpdate, RFC 8446 4.6.3).
    func KeyUpdate() Task<void!> {
        return this.c.keyUpdate(true)
    }
}

// The shared state of a connection.
struct conn {
    raw      std.net.Conn
    isClient bool
    cfg      Config
    srv      *serverState
    // record layer
    inbuf []byte
    rd    cipherState
    wr    cipherState
    // TLS 1.2: keys that take effect at the next change_cipher_spec
    pendRd     cipherState
    hsData     []byte
    appData    []byte
    out        []byte
    transcript []byte
    vers       int
    // state
    started   bool
    done      bool
    hsErr     error
    rdErr     error
    closed    bool
    sentClose bool
    gotClose  bool
    alert     int
    hsMu      std.task.Mutex
    wMu       std.task.Mutex
    // results
    suite      suiteInfo
    alpn       string
    serverName string
    peerCerts  []*std.crypto.x509.Certificate
    verified   []*std.crypto.x509.Certificate
    resumed    bool
    // TLS 1.3 secrets kept after the handshake
    resMaster []byte
    // TLS 1.2
    master       []byte
    clientRandom []byte
    serverRandom []byte
    sessionKey   string

    func state() ConnectionState {
        return ConnectionState{
            Version: this.vers,
            CipherSuite: this.suite.id,
            CipherSuiteName: this.suite.name,
            ALPN: this.alpn,
            ServerName: this.serverName,
            PeerCertificates: this.peerCerts,
            VerifiedChain: this.verified,
            Resumed: this.resumed,
            HandshakeComplete: this.done,
        }
    }

    // Records the alert to send for err and returns err.
    func fail(desc int, msg string) error {
        if this.alert == 0 {
            this.alert = desc
        }
        return error("tls: " + msg)
    }

    // ---- raw input ----

    async func fill() void! {
        var b = await this.raw.Read(18437)?
        if len(b) == 0 {
            return error("tls: unexpected end of stream")
        }
        this.inbuf = cat(this.inbuf, b)
    }

    // Reads one record and removes its protection.
    async func readRecord() record! {
        for len(this.inbuf) < 5 {
            if len(this.inbuf) == 0 {
                // a clean end of stream between records
                var b = await this.raw.Read(18437)?
                if len(b) == 0 {
                    return record{typ: -1}
                }
                this.inbuf = cat(this.inbuf, b)
            } else {
                await this.fill()?
            }
        }
        var typ = int(this.inbuf[0])
        var n = u16(this.inbuf, 3)
        if typ < 20 || typ > 23 {
            return this.fail(10, std.fmt.Format("invalid record type {0} (not a TLS peer?)", typ))
        }
        if int(this.inbuf[1]) != 3 {
            return this.fail(70, "invalid record version")
        }
        if n > 16384 + 2048 {
            return this.fail(22, "record too large")
        }
        for len(this.inbuf) < 5 + n {
            await this.fill()?
        }
        var hdr = this.inbuf[..5]
        var body = this.inbuf[5..5 + n]
        this.inbuf = this.inbuf[5 + n..]
        var r = try openRecord(&this.rd, hdr, body) catch err {
            return this.fail(20, "bad record MAC")
        }
        if len(r.data) > 16384 {
            return this.fail(22, "record overflow")
        }
        return r
    }

    // Reads the next handshake message: type, body and the whole message.
    async func readHandshake() hsMsg! {
        for {
            if len(this.hsData) >= 4 {
                var n = u24(this.hsData, 1)
                if n > 65536 * 4 {
                    return this.fail(50, "handshake message too large")
                }
                if len(this.hsData) >= 4 + n {
                    var whole = this.hsData[..4 + n]
                    this.hsData = this.hsData[4 + n..]
                    return hsMsg{typ: int(whole[0]), body: whole[4..], raw: whole}
                }
            }
            var r = await this.readRecord()?
            if r.typ == -1 {
                return error("tls: connection closed during the handshake")
            }
            if r.typ == 22 {
                this.hsData = cat(this.hsData, r.data)
            } else if r.typ == 21 {
                return this.alertErr(r.data)
            } else if r.typ == 20 {
                if len(this.hsData) > 0 || len(r.data) != 1 || int(r.data[0]) != 1 {
                    return this.fail(10, "unexpected change_cipher_spec")
                }
                if this.vers == 771 || (this.vers == 0 && this.pendRd.active()) {
                    if !this.pendRd.active() {
                        return this.fail(10, "unexpected change_cipher_spec")
                    }
                    this.rd = this.pendRd
                    this.pendRd = cipherState{}
                }
                // TLS 1.3 middlebox-compatibility CCS: ignored
            } else {
                return this.fail(10, "unexpected application data during the handshake")
            }
        }
    }

    // The next handshake message, which must be of type want.
    async func expect(want int, what string) hsMsg! {
        var m = await this.readHandshake()?
        if m.typ != want {
            return this.fail(
                10,
                std.fmt.Format("unexpected handshake message {0} (want {1})", m.typ, what),
            )
        }
        return m
    }

    func alertErr(data []byte) error {
        if len(data) == 2 && int(data[1]) == 0 {
            this.gotClose = true
            return error("tls: connection closed by the peer (close_notify)")
        }
        var d = 0
        if len(data) == 2 {
            d = int(data[1])
        }
        this.alert = -1
        return error("tls: remote error: " + alertText(d))
    }

    // ---- output ----

    // Protects data as records of type typ into the output buffer.
    func queue(typ int, data []byte) {
        var off = 0
        for {
            var end = off + 16384
            if end > len(data) {
                end = len(data)
            }
            this.out = cat(this.out, sealRecord(&this.wr, typ, data[off..end]))
            off = end
            if off >= len(data) {
                break
            }
        }
    }

    // Queues a handshake message and adds it to the transcript.
    func queueHandshake(typ int, body []byte) {
        var b = newBuilder()
        b.u8(typ)
        b.vec24(body)
        var msg = b.bytes()
        this.transcript = cat(this.transcript, msg)
        this.queue(22, msg)
    }

    async func flush() void! {
        if len(this.out) == 0 {
            return
        }
        var data = this.out
        var none []byte = []
        this.out = none
        _ = await this.raw.Write(data)?
    }

    func addTranscript(m hsMsg) {
        this.transcript = cat(this.transcript, m.raw)
    }

    func transcriptHash() []byte! {
        return hashOf(this.suite.hash, this.transcript)?
    }

    // Best-effort fatal alert (unprotected before keys exist).
    async func sendAlert(desc int) {
        if this.sentClose || this.closed {
            return
        }
        this.sentClose = true
        var none []byte = []
        this.out = none
        var level = 2
        if desc == 0 || desc == 90 {
            level = 1
        }
        this.queue(21, [byte(level), byte(desc)])
        try await this.flush() catch err {
            this.rdErr = err
        }
    }

    // ---- handshake driver ----

    async func handshake() void! {
        if this.done {
            return
        }
        await this.hsMu.Lock()
        if this.done || this.hsErr != nil {
            this.hsMu.Unlock()
            if this.hsErr != nil {
                return this.hsErr
            }
            return
        }
        this.started = true
        var err error = nil
        if this.isClient {
            try await runClient(this) catch e {
                err = e
            }
        } else {
            try await runServer(this) catch e {
                err = e
            }
        }
        if err != nil {
            this.hsErr = err
            var desc = this.alert
            if desc == 0 {
                desc = 40
            }
            if desc > 0 {
                await this.sendAlert(desc)
            }
            this.closeRaw()
            this.hsMu.Unlock()
            return err
        }
        this.done = true
        this.hsMu.Unlock()
    }

    // ---- application data ----

    async func read(n int) []byte! {
        await this.handshake()?
        for len(this.appData) == 0 {
            if this.rdErr != nil {
                return this.rdErr
            }
            if this.gotClose || this.closed {
                var none []byte = []
                return none
            }
            var r = try await this.readRecord() catch err {
                this.rdErr = err
                return err
            }
            if r.typ == -1 {
                // end of stream without close_notify: treated as EOF
                this.gotClose = true
            } else if r.typ == 23 {
                this.appData = r.data
            } else if r.typ == 21 {
                var e = this.alertErr(r.data)
                if !this.gotClose {
                    this.rdErr = e
                    return e
                }
            } else if r.typ == 22 {
                this.hsData = cat(this.hsData, r.data)
                try await this.postHandshake() catch err {
                    this.rdErr = err
                    return err
                }
            } else {
                this.rdErr = this.fail(10, "unexpected record after the handshake")
                return this.rdErr
            }
        }
        var k = n
        if k <= 0 || k > len(this.appData) {
            k = len(this.appData)
        }
        var out = this.appData[..k]
        this.appData = this.appData[k..]
        return out
    }

    async func readString(n int) string! {
        var b = await this.read(n)?
        return std.strings.FromBytes(b)
    }

    // Handles buffered post-handshake messages.
    async func postHandshake() void! {
        for len(this.hsData) >= 4 {
            var n = u24(this.hsData, 1)
            if len(this.hsData) < 4 + n {
                return
            }
            var whole = this.hsData[..4 + n]
            this.hsData = this.hsData[4 + n..]
            var m = hsMsg{typ: int(whole[0]), body: whole[4..], raw: whole}
            if this.vers == 772 {
                if m.typ == 4 && this.isClient {
                    handleTicket13(this, m)?
                } else if m.typ == 24 {
                    var p = newParser(m.body)
                    var req = p.u8()
                    if !p.ok() || !p.empty() || req > 1 {
                        return this.fail(50, "malformed KeyUpdate")
                    }
                    var next = ExpandLabel(
                        this.rd.suite.hash,
                        this.rd.secret,
                        "traffic upd",
                        [],
                        hashLen(this.rd.suite.hash),
                    )?
                    this.rd = cipher13(this.rd.suite, next)?
                    if req == 1 {
                        await this.keyUpdate(false)?
                    }
                } else {
                    return this.fail(
                        10,
                        std.fmt.Format("unexpected post-handshake message {0}", m.typ),
                    )
                }
            } else if m.typ == 0 && this.isClient {
                // TLS 1.2 HelloRequest: renegotiation is refused with a warning
                this.queue(21, [byte(1), byte(100)])
                await this.flush()?
            } else {
                return this.fail(10, "renegotiation is not supported")
            }
        }
    }

    async func keyUpdate(requestPeer bool) void! {
        await this.handshake()?
        if this.vers != 772 {
            return error("tls: KeyUpdate needs TLS 1.3")
        }
        await this.wMu.Lock()
        var req = 0
        if requestPeer {
            req = 1
        }
        var b = newBuilder()
        b.u8(24)
        b.u24(1)
        b.u8(req)
        this.queue(22, b.bytes())
        var next = try ExpandLabel(
            this.wr.suite.hash,
            this.wr.secret,
            "traffic upd",
            [],
            hashLen(this.wr.suite.hash),
        ) catch err {
            this.wMu.Unlock()
            return err
        }
        this.wr = try cipher13(this.wr.suite, next) catch err {
            this.wMu.Unlock()
            return err
        }
        try await this.flush() catch err {
            this.wMu.Unlock()
            return err
        }
        this.wMu.Unlock()
    }

    async func write(data []byte) int! {
        await this.handshake()?
        if this.closed || this.sentClose {
            return error("tls: use of closed connection")
        }
        await this.wMu.Lock()
        this.queue(23, data)
        try await this.flush() catch err {
            this.wMu.Unlock()
            return err
        }
        this.wMu.Unlock()
        return len(data)
    }

    func closeRaw() {
        this.closed = true
        this.raw.Close()
    }

    func close() {
        if this.closed {
            return
        }
        if this.done && !this.sentClose {
            this.sentClose = true
            this.queue(21, [byte(1), byte(0)])
            var data = this.out
            var none []byte = []
            this.out = none
            // the write completes at once when the socket is writable; a
            // stalled one is abandoned when the socket closes below
            _ = this.raw.Write(data)
        }
        this.closeRaw()
    }
}

// A handshake message.
struct hsMsg {
    typ  int
    body []byte
    raw  []byte
}

func newConn(raw std.net.Conn, client bool) *conn {
    var none []byte = []
    return new conn{raw: raw, isClient: client, inbuf: none, transcript: none}
}

// Client wraps an established TCP connection as the client side of TLS
// and runs the handshake.
async func Client(raw std.net.Conn, cfg Config) Conn! {
    var c = newConn(raw, true)
    c.cfg = cfg
    c.serverName = cfg.ServerName
    await c.handshake()?
    return Conn{c: c}
}

// Dial connects to addr ("host:port") and runs a TLS client handshake.
async func Dial(addr string, cfg Config) Conn! {
    return await DialTimeout(addr, cfg, 0)?
}

// DialTimeout is Dial with a timeout (ms, 0 = none) for the connect and
// for every network operation of the handshake.
async func DialTimeout(addr string, cfg Config, ms int) Conn! {
    var hp = std.net.SplitHostPort(addr)?
    var conf = cfg
    if conf.ServerName == "" {
        conf.ServerName = hp.Host
    }
    var raw = await std.net.DialTimeout(addr, ms)?
    raw.SetTimeout(ms)
    var c = newConn(raw, true)
    c.cfg = conf
    c.serverName = conf.ServerName
    await c.handshake()?
    raw.SetTimeout(0)
    return Conn{c: c}
}

// Server-side configuration shared by the connections of a Listener.
struct serverState {
    cfg       ServerConfig
    ticketKey []byte
}

func newServerState(cfg ServerConfig) *serverState! {
    var key = cfg.TicketKey
    if len(key) != 32 {
        key = randomBytes(32)?
    }
    return new serverState{cfg: cfg, ticketKey: key}
}

// ServerConn wraps an accepted TCP connection as the server side of TLS;
// the handshake runs on first use (or Handshake()).
func ServerConn(raw std.net.Conn, cfg ServerConfig) Conn! {
    var c = newConn(raw, false)
    c.srv = newServerState(cfg)?
    return Conn{c: c}
}

// Listener accepts TLS connections from a TCP listener.
struct Listener {
    l  std.net.Listener
    st *serverState

    // Waits for the next TCP connection; its handshake runs on first use.
    func Accept() Task<Conn!> {
        return acceptTLS(this.l, this.st)
    }

    // Close stops listening.
    func Close() {
        this.l.Close()
    }

    // Addr returns the bound "ip:port".
    func Addr() string {
        return this.l.Addr()
    }

    // Port returns the bound port.
    func Port() int {
        return this.l.Port()
    }
}

async func acceptTLS(l std.net.Listener, st *serverState) Conn! {
    var raw = await l.Accept()?
    var c = newConn(raw, false)
    c.srv = st
    return Conn{c: c}
}

// Server makes a TLS listener from a TCP listener.
func Server(l std.net.Listener, cfg ServerConfig) Listener! {
    if len(cfg.Certificates) == 0 {
        return error("tls: ServerConfig has no certificates")
    }
    return Listener{l: l, st: newServerState(cfg)?}
}

// Listen listens on addr ("host:port") and returns a TLS listener.
func Listen(addr string, cfg ServerConfig) Listener! {
    var l = std.net.Listen(addr)?
    return Server(l, cfg)?
}
