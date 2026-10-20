/* api.c - the Slang-facing wrappers of the crypto primitives:
 * std.crypto.{aes, chacha, x25519, ecdh, ecdsa, ed25519, rsa, hkdf}, the
 * additions to std.crypto.{hash, hmac}, and the internal rt* helpers of
 * std.crypto.x509 and std.net.tls (runtime/ABI.md section 19). Calling
 * convention of ABI.md 14.1: []byte arguments are borrowed slices (NULL =
 * empty), []byte results go to a trailing out-slice, T! adds a trailing
 * slang_error **. */
#include "../rt_internal.h"
#include "../platform/platform.h"
#include "crypto.h"
#include "slang_crypto.h"

#include <stdio.h>
#include <string.h>

static inline int64_t blen(const slang_slice *s) { return s ? s->len : 0; }
static inline const uint8_t *bdata(const slang_slice *s) {
    return s && s->len ? (const uint8_t *)s->data : (const uint8_t *)"";
}
static inline int64_t slen_(const slang_string *s) { return s ? s->len : 0; }
static inline const char *sdata(const slang_string *s) { return s ? s->data : ""; }

static void set_err(slang_error **err, const char *msg) {
    if (err) *err = slang_error_new_cstr(msg);
}

/* New []byte of n bytes (zeroed) in *out; returns its data. */
static uint8_t *new_bytes(slang_slice *out, int64_t n) {
    slang_slice_new(out, 1, n, n);
    return n ? (uint8_t *)out->data : NULL;
}

static void bytes_out(slang_slice *out, const uint8_t *d, int64_t n) {
    slang_slice_from(out, 1, NULL, d, n);
}

static int hash_alg(const slang_string *name, slang_error **err) {
    int alg = slang__hash_by_name(sdata(name), (size_t)slen_(name));
    if (!alg) {
        char buf[96];
        snprintf(buf, sizeof buf, "crypto: unknown hash \"%.40s\" (want sha256, sha384, sha512 or sha1)", sdata(name));
        set_err(err, buf);
    }
    return alg;
}

/* ---- std.crypto.hash / hmac additions ---- */

void slang_crypto_hash_sha384(const slang_string *data, slang_slice *out) {
    uint8_t d[48];
    slang__hash(SLANG_HASH_SHA384, (const uint8_t *)sdata(data), (size_t)slen_(data), d);
    bytes_out(out, d, 48);
}

slang_string *slang_crypto_hash_sha384_hex(const slang_string *data) {
    static const char hx[] = "0123456789abcdef";
    uint8_t d[48];
    slang__hash(SLANG_HASH_SHA384, (const uint8_t *)sdata(data), (size_t)slen_(data), d);
    slang_string *r = slang__string_alloc(96);
    for (int i = 0; i < 48; i++) {
        r->data[2 * i] = hx[d[i] >> 4];
        r->data[2 * i + 1] = hx[d[i] & 15];
    }
    return r;
}

void slang_crypto_hash_sum(const slang_string *alg, const slang_slice *data, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    int a = hash_alg(alg, err);
    if (!a) return;
    uint8_t d[64];
    slang__hash(a, bdata(data), (size_t)blen(data), d);
    bytes_out(out, d, (int64_t)slang__hash_size(a));
}

/* Streaming state kept in a []byte (an slang_hash_ctx image). */
void slang_crypto_hash_rt_new(const slang_string *alg, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    int a = hash_alg(alg, err);
    if (!a) return;
    slang_hash_ctx c;
    slang__hash_init(&c, a);
    bytes_out(out, (const uint8_t *)&c, (int64_t)sizeof c);
}

static bool load_hash(const slang_slice *st, slang_hash_ctx *c) {
    if (blen(st) != (int64_t)sizeof *c) return false;
    memcpy(c, st->data, sizeof *c);
    return slang__hash_size((int)c->alg) != 0;
}

void slang_crypto_hash_rt_write(const slang_slice *st, const slang_slice *data) {
    slang_hash_ctx c;
    if (!load_hash(st, &c)) slang__panicf("crypto/hash: invalid hasher state");
    slang__hash_update(&c, bdata(data), (size_t)blen(data));
    memcpy(st->data, &c, sizeof c);
}

void slang_crypto_hash_rt_sum(const slang_slice *st, slang_slice *out) {
    slang_hash_ctx c;
    if (!load_hash(st, &c)) slang__panicf("crypto/hash: invalid hasher state");
    uint8_t d[64];
    slang__hash_final(&c, d);
    bytes_out(out, d, (int64_t)slang__hash_size((int)c.alg));
}

void slang_crypto_hmac_sum(const slang_string *alg, const slang_slice *key, const slang_slice *data, slang_slice *out,
                           slang_error **err) {
    memset(out, 0, sizeof *out);
    int a = hash_alg(alg, err);
    if (!a) return;
    uint8_t d[64];
    slang__hmac(a, bdata(key), (size_t)blen(key), bdata(data), (size_t)blen(data), d);
    bytes_out(out, d, (int64_t)slang__hash_size(a));
}

/* ---- std.crypto.hkdf ---- */

void slang_crypto_hkdf_extract(const slang_string *alg, const slang_slice *salt, const slang_slice *ikm,
                               slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    int a = hash_alg(alg, err);
    if (!a) return;
    uint8_t prk[64];
    slang__hkdf_extract(a, bdata(salt), (size_t)blen(salt), bdata(ikm), (size_t)blen(ikm), prk);
    bytes_out(out, prk, (int64_t)slang__hash_size(a));
    slang__crypto_wipe(prk, sizeof prk);
}

void slang_crypto_hkdf_expand(const slang_string *alg, const slang_slice *prk, const slang_slice *info, int64_t n,
                              slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    int a = hash_alg(alg, err);
    if (!a) return;
    if (n < 0 || n > 255 * (int64_t)slang__hash_size(a)) {
        set_err(err, "hkdf: requested length out of range");
        return;
    }
    uint8_t *p = new_bytes(out, n);
    if (n) slang__hkdf_expand(a, bdata(prk), (size_t)blen(prk), bdata(info), (size_t)blen(info), p, (size_t)n);
}

/* ---- AEADs ---- */

static bool check_aead(const slang_slice *nonce, const char *who, slang_error **err) {
    if (blen(nonce) != 12) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s: nonce must be 12 bytes", who);
        set_err(err, buf);
        return false;
    }
    return true;
}

static void auth_failed(const char *who, slang_error **err) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s: message authentication failed", who);
    set_err(err, buf);
}

void slang_crypto_aes_gcm_seal(const slang_slice *key, const slang_slice *nonce, const slang_slice *pt,
                               const slang_slice *aad, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    slang_gcm_ctx c;
    if (!slang__gcm_init(&c, bdata(key), (size_t)blen(key))) {
        set_err(err, "aes: invalid key size (want 16, 24 or 32 bytes)");
        return;
    }
    if (!check_aead(nonce, "aes-gcm", err)) return;
    uint8_t *p = new_bytes(out, blen(pt) + 16);
    slang__gcm_seal(&c, bdata(nonce), bdata(aad), (size_t)blen(aad), bdata(pt), (size_t)blen(pt), p);
    slang__crypto_wipe(&c, sizeof c);
}

void slang_crypto_aes_gcm_open(const slang_slice *key, const slang_slice *nonce, const slang_slice *ct,
                               const slang_slice *aad, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    slang_gcm_ctx c;
    if (!slang__gcm_init(&c, bdata(key), (size_t)blen(key))) {
        set_err(err, "aes: invalid key size (want 16, 24 or 32 bytes)");
        return;
    }
    if (!check_aead(nonce, "aes-gcm", err)) return;
    if (blen(ct) < 16) {
        auth_failed("aes-gcm", err);
        return;
    }
    int64_t n = blen(ct) - 16;
    uint8_t *p = new_bytes(out, n);
    uint8_t dummy[1];
    if (!slang__gcm_open(&c, bdata(nonce), bdata(aad), (size_t)blen(aad), bdata(ct), (size_t)n, p ? p : dummy)) {
        slang_slice_release(out);
        auth_failed("aes-gcm", err);
    }
    slang__crypto_wipe(&c, sizeof c);
}

void slang_crypto_aes_encrypt_block(const slang_slice *key, const slang_slice *block, slang_slice *out,
                                    slang_error **err) {
    memset(out, 0, sizeof *out);
    slang_aes_key k;
    if (!slang__aes_init(&k, bdata(key), (size_t)blen(key))) {
        set_err(err, "aes: invalid key size (want 16, 24 or 32 bytes)");
        return;
    }
    if (blen(block) != 16) {
        set_err(err, "aes: block must be 16 bytes");
        return;
    }
    slang__aes_encrypt_block(&k, bdata(block), new_bytes(out, 16));
    slang__crypto_wipe(&k, sizeof k);
}

bool slang_crypto_aes_has_hardware(void) { return slang__cpu_has_aesni(); }

void slang_crypto_chacha_seal(const slang_slice *key, const slang_slice *nonce, const slang_slice *pt,
                              const slang_slice *aad, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(key) != 32) {
        set_err(err, "chacha20poly1305: key must be 32 bytes");
        return;
    }
    if (!check_aead(nonce, "chacha20poly1305", err)) return;
    uint8_t *p = new_bytes(out, blen(pt) + 16);
    slang__chacha20poly1305_seal(bdata(key), bdata(nonce), bdata(aad), (size_t)blen(aad), bdata(pt),
                                 (size_t)blen(pt), p);
}

void slang_crypto_chacha_open(const slang_slice *key, const slang_slice *nonce, const slang_slice *ct,
                              const slang_slice *aad, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(key) != 32) {
        set_err(err, "chacha20poly1305: key must be 32 bytes");
        return;
    }
    if (!check_aead(nonce, "chacha20poly1305", err)) return;
    if (blen(ct) < 16) {
        auth_failed("chacha20poly1305", err);
        return;
    }
    int64_t n = blen(ct) - 16;
    uint8_t *p = new_bytes(out, n), dummy[1];
    if (!slang__chacha20poly1305_open(bdata(key), bdata(nonce), bdata(aad), (size_t)blen(aad), bdata(ct), (size_t)n,
                                      p ? p : dummy)) {
        slang_slice_release(out);
        auth_failed("chacha20poly1305", err);
    }
}

/* ---- key generation helpers ---- */

static void random_out(slang_slice *out, int64_t n, slang_error **err) {
    uint8_t *p = new_bytes(out, n);
    if (!slang__crypto_random(p, (size_t)n)) {
        slang_slice_release(out);
        set_err(err, "crypto/rand: entropy source failed");
    }
}

/* ---- std.crypto.x25519 ---- */

void slang_crypto_x25519_generate_key(slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    random_out(out, 32, err);
}

void slang_crypto_x25519_public_key(const slang_slice *priv, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(priv) != 32) {
        set_err(err, "x25519: private key must be 32 bytes");
        return;
    }
    slang__x25519_base(new_bytes(out, 32), bdata(priv));
}

void slang_crypto_x25519_shared_secret(const slang_slice *priv, const slang_slice *peer, slang_slice *out,
                                       slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(priv) != 32 || blen(peer) != 32) {
        set_err(err, "x25519: keys must be 32 bytes");
        return;
    }
    if (!slang__x25519(new_bytes(out, 32), bdata(priv), bdata(peer))) {
        slang_slice_release(out);
        set_err(err, "x25519: low-order public key (all-zero shared secret)");
    }
}

/* ---- std.crypto.ecdh (P-256) ---- */

void slang_crypto_ecdh_generate_key(slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    uint8_t *p = new_bytes(out, 32);
    if (!slang__p256_genkey(p)) {
        slang_slice_release(out);
        set_err(err, "crypto/rand: entropy source failed");
    }
}

void slang_crypto_ecdh_public_key(const slang_slice *priv, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(priv) != 32 || !slang__p256_pubkey(new_bytes(out, 65), bdata(priv))) {
        slang_slice_release(out);
        set_err(err, "p256: invalid private key");
    }
}

void slang_crypto_ecdh_shared_secret(const slang_slice *priv, const slang_slice *peer, slang_slice *out,
                                     slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(priv) != 32 || blen(peer) != 65) {
        set_err(err, "p256: invalid key (want a 32-byte private key and a 65-byte uncompressed point)");
        return;
    }
    if (!slang__p256_ecdh(new_bytes(out, 32), bdata(priv), bdata(peer))) {
        slang_slice_release(out);
        set_err(err, "p256: invalid peer public key");
    }
}

/* ---- std.crypto.ecdsa (P-256, ASN.1 DER signatures) ---- */

void slang_crypto_ecdsa_generate_key(slang_slice *out, slang_error **err) { slang_crypto_ecdh_generate_key(out, err); }

void slang_crypto_ecdsa_public_key(const slang_slice *priv, slang_slice *out, slang_error **err) {
    slang_crypto_ecdh_public_key(priv, out, err);
}

/* DER INTEGER of a 32-byte big-endian value; returns bytes written. */
static size_t der_int(uint8_t *o, const uint8_t v[32]) {
    size_t i = 0;
    while (i < 31 && v[i] == 0) i++;
    size_t n = 32 - i, pad = v[i] & 0x80 ? 1 : 0;
    o[0] = 0x02;
    o[1] = (uint8_t)(n + pad);
    if (pad) o[2] = 0;
    memcpy(o + 2 + pad, v + i, n);
    return 2 + pad + n;
}

void slang_crypto_ecdsa_sign(const slang_slice *priv, const slang_slice *digest, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    uint8_t sig[64], extra[32];
    if (blen(priv) != 32) {
        set_err(err, "ecdsa: private key must be 32 bytes");
        return;
    }
    if (!slang__crypto_random(extra, 32) ||
        !slang__p256_sign(sig, bdata(priv), bdata(digest), (size_t)blen(digest), extra, 32)) {
        set_err(err, "ecdsa: signing failed (invalid key)");
        return;
    }
    uint8_t der[72];
    size_t n = der_int(der + 2, sig);
    n += der_int(der + 2 + n, sig + 32);
    der[0] = 0x30;
    der[1] = (uint8_t)n;
    bytes_out(out, der, (int64_t)n + 2);
}

/* Strict DER ECDSA-Sig-Value -> r||s (32 bytes each). */
static bool der_sig_parse(const uint8_t *p, size_t n, uint8_t rs[64]) {
    if (n < 8 || p[0] != 0x30 || p[1] != n - 2 || p[1] & 0x80) return false;
    size_t off = 2;
    memset(rs, 0, 64);
    for (int k = 0; k < 2; k++) {
        if (off + 2 > n || p[off] != 0x02) return false;
        size_t l = p[off + 1];
        off += 2;
        if (l == 0 || l > 33 || off + l > n) return false;
        const uint8_t *v = p + off;
        if (v[0] & 0x80) return false;                         /* negative */
        if (l > 1 && v[0] == 0 && !(v[1] & 0x80)) return false; /* non-minimal */
        if (l == 33) {
            if (v[0] != 0) return false;
            v++;
            l--;
        }
        memcpy(rs + 32 * k + (32 - l), v, l);
        off += p[off - 1];
    }
    return off == n;
}

bool slang_crypto_ecdsa_verify(const slang_slice *pub, const slang_slice *digest, const slang_slice *sig) {
    uint8_t rs[64];
    if (blen(pub) != 65 || !der_sig_parse(bdata(sig), (size_t)blen(sig), rs)) return false;
    return slang__p256_verify(rs, bdata(pub), bdata(digest), (size_t)blen(digest));
}

/* ---- std.crypto.ed25519 ---- */

void slang_crypto_ed25519_generate_key(slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    random_out(out, 32, err);
}

void slang_crypto_ed25519_public_key(const slang_slice *seed, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(seed) != 32) {
        set_err(err, "ed25519: private key (seed) must be 32 bytes");
        return;
    }
    slang__ed25519_pubkey(new_bytes(out, 32), bdata(seed));
}

void slang_crypto_ed25519_sign(const slang_slice *seed, const slang_slice *msg, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(seed) != 32) {
        set_err(err, "ed25519: private key (seed) must be 32 bytes");
        return;
    }
    slang__ed25519_sign(new_bytes(out, 64), bdata(seed), bdata(msg), (size_t)blen(msg));
}

bool slang_crypto_ed25519_verify(const slang_slice *pub, const slang_slice *msg, const slang_slice *sig) {
    if (blen(pub) != 32 || blen(sig) != 64) return false;
    return slang__ed25519_verify(bdata(sig), bdata(pub), bdata(msg), (size_t)blen(msg));
}

/* ---- std.crypto.rsa ---- */

bool slang_crypto_rsa_verify_pkcs1v15(const slang_slice *n, const slang_slice *e, const slang_string *hash,
                                      const slang_slice *digest, const slang_slice *sig) {
    int a = slang__hash_by_name(sdata(hash), (size_t)slen_(hash));
    return a && slang__rsa_verify_pkcs1(bdata(n), (size_t)blen(n), bdata(e), (size_t)blen(e), a, bdata(digest),
                                        (size_t)blen(digest), bdata(sig), (size_t)blen(sig));
}

bool slang_crypto_rsa_verify_pss(const slang_slice *n, const slang_slice *e, const slang_string *hash,
                                 const slang_slice *digest, const slang_slice *sig) {
    int a = slang__hash_by_name(sdata(hash), (size_t)slen_(hash));
    return a && slang__rsa_verify_pss(bdata(n), (size_t)blen(n), bdata(e), (size_t)blen(e), a, bdata(digest),
                                      (size_t)blen(digest), bdata(sig), (size_t)blen(sig), -1);
}

static void rsa_sign(bool pss, const slang_slice *key, const slang_string *hash, const slang_slice *digest,
                     slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    int a = hash_alg(hash, err);
    if (!a) return;
    size_t k = slang__rsa_priv_size(bdata(key), (size_t)blen(key));
    if (k == 0 || k > 1024) {
        set_err(err, "rsa: invalid PKCS#1 private key");
        return;
    }
    uint8_t *p = new_bytes(out, (int64_t)k);
    size_t sl = 0;
    bool ok = pss ? slang__rsa_sign_pss(bdata(key), (size_t)blen(key), a, bdata(digest), (size_t)blen(digest), p, &sl)
                  : slang__rsa_sign_pkcs1(bdata(key), (size_t)blen(key), a, bdata(digest), (size_t)blen(digest), p, &sl);
    if (!ok) {
        slang_slice_release(out);
        set_err(err, "rsa: signing failed (bad key, digest size or key too small for the hash)");
    }
}

void slang_crypto_rsa_sign_pkcs1v15(const slang_slice *key, const slang_string *hash, const slang_slice *digest,
                                    slang_slice *out, slang_error **err) {
    rsa_sign(false, key, hash, digest, out, err);
}

void slang_crypto_rsa_sign_pss(const slang_slice *key, const slang_string *hash, const slang_slice *digest,
                               slang_slice *out, slang_error **err) {
    rsa_sign(true, key, hash, digest, out, err);
}

/* ---- std.crypto.x509 internals ---- */

slang_string *slang_crypto_x509_rt_system_roots_path(void) {
    const char *p = slang__plat_cert_bundle_path();
    return slang_string_new(p ? p : "", p ? (int64_t)strlen(p) : 0);
}

/* ---- std.net.tls internals ---- */

void slang_net_tls_rt_concat(const slang_slice *a, const slang_slice *b, slang_slice *out) {
    int64_t na = blen(a), nb = blen(b);
    uint8_t *p = new_bytes(out, na + nb);
    if (na) memcpy(p, a->data, (size_t)na);
    if (nb) memcpy(p + na, b->data, (size_t)nb);
}

void slang_net_tls_rt_zeros(int64_t n, slang_slice *out) {
    if (n < 0) n = 0;
    new_bytes(out, n);
}

/* dst[off..] = src (bounds-checked). */
void slang_net_tls_rt_copy(const slang_slice *dst, int64_t off, const slang_slice *src) {
    int64_t n = blen(src);
    if (off < 0 || off + n > blen(dst)) slang__panicf("tls: copy out of range");
    if (n) memmove((uint8_t *)dst->data + off, src->data, (size_t)n);
}

/* AEAD context: byte 0 = kind (1 AES-GCM, 2 ChaCha20-Poly1305), then the
 * slang_gcm_ctx image or the 32-byte key. */
void slang_net_tls_rt_aead_new(int64_t kind, const slang_slice *key, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (kind == 1) {
        slang_gcm_ctx c;
        if (!slang__gcm_init(&c, bdata(key), (size_t)blen(key))) {
            set_err(err, "tls: invalid AES key");
            return;
        }
        uint8_t *p = new_bytes(out, 1 + (int64_t)sizeof c);
        p[0] = 1;
        memcpy(p + 1, &c, sizeof c);
        slang__crypto_wipe(&c, sizeof c);
    } else if (kind == 2 && blen(key) == 32) {
        uint8_t *p = new_bytes(out, 33);
        p[0] = 2;
        memcpy(p + 1, key->data, 32);
    } else {
        set_err(err, "tls: invalid AEAD");
    }
}

static int aead_kind(const slang_slice *ctx) {
    int64_t n = blen(ctx);
    const uint8_t *p = bdata(ctx);
    if (n == 1 + (int64_t)sizeof(slang_gcm_ctx) && p[0] == 1) return 1;
    if (n == 33 && p[0] == 2) return 2;
    slang__panicf("tls: invalid AEAD context");
}

void slang_net_tls_rt_aead_seal(const slang_slice *ctx, const slang_slice *nonce, const slang_slice *aad,
                                const slang_slice *pt, slang_slice *out) {
    if (blen(nonce) != 12) slang__panicf("tls: nonce must be 12 bytes");
    int kind = aead_kind(ctx);
    uint8_t *p = new_bytes(out, blen(pt) + 16);
    if (kind == 1) {
        slang_gcm_ctx c;
        memcpy(&c, (const uint8_t *)ctx->data + 1, sizeof c);
        slang__gcm_seal(&c, bdata(nonce), bdata(aad), (size_t)blen(aad), bdata(pt), (size_t)blen(pt), p);
        slang__crypto_wipe(&c, sizeof c);
    } else {
        slang__chacha20poly1305_seal((const uint8_t *)ctx->data + 1, bdata(nonce), bdata(aad), (size_t)blen(aad), bdata(pt),
                                     (size_t)blen(pt), p);
    }
}

void slang_net_tls_rt_aead_open(const slang_slice *ctx, const slang_slice *nonce, const slang_slice *aad,
                                const slang_slice *ct, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(nonce) != 12) slang__panicf("tls: nonce must be 12 bytes");
    int kind = aead_kind(ctx);
    if (blen(ct) < 16) {
        set_err(err, "tls: bad record MAC");
        return;
    }
    int64_t n = blen(ct) - 16;
    uint8_t *p = new_bytes(out, n), dummy[1];
    bool ok;
    if (kind == 1) {
        slang_gcm_ctx c;
        memcpy(&c, (const uint8_t *)ctx->data + 1, sizeof c);
        ok = slang__gcm_open(&c, bdata(nonce), bdata(aad), (size_t)blen(aad), bdata(ct), (size_t)n, p ? p : dummy);
        slang__crypto_wipe(&c, sizeof c);
    } else {
        ok = slang__chacha20poly1305_open((const uint8_t *)ctx->data + 1, bdata(nonce), bdata(aad), (size_t)blen(aad), bdata(ct),
                                          (size_t)n, p ? p : dummy);
    }
    if (!ok) {
        slang_slice_release(out);
        set_err(err, "tls: bad record MAC");
    }
}
