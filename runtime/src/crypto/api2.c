/* api2.c - Slang-facing wrappers of the tier-3 crypto additions
 * (runtime/ABI.md section 23): std.crypto.kdf (PBKDF2, scrypt; Argon2 is
 * in ../std_argon2.c), std.crypto.hash.Blake2b, P-384 in std.crypto.ecdh /
 * std.crypto.ecdsa, AES-CBC and PKCS#7 in std.crypto.aes, RSA-OAEP and
 * prime generation in std.crypto.rsa. Conventions of api.c (ABI.md 14.1). */
#include "../rt_internal.h"
#include "crypto.h"

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
        snprintf(buf, sizeof buf, "crypto: unknown hash \"%.*s\" (want sha256, sha384, sha512 or sha1)", (int)(slen_(name) < 40 ? slen_(name) : 40), sdata(name));
        set_err(err, buf);
    }
    return alg;
}

/* ---- std.crypto.kdf ---- */

void slang_crypto_kdf_pbkdf2(const slang_slice *pw, const slang_slice *salt, int64_t iter, int64_t n,
                             const slang_string *hash, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    int a = hash_alg(hash, err);
    if (!a) return;
    if (iter < 1 || n < 1 || n > ((int64_t)1 << 30)) {
        set_err(err, "pbkdf2: iterations and key length must be positive");
        return;
    }
    slang__pbkdf2(a, bdata(pw), (size_t)blen(pw), bdata(salt), (size_t)blen(salt), (uint64_t)iter, new_bytes(out, n),
                  (size_t)n);
}

void slang_crypto_kdf_scrypt(const slang_slice *pw, const slang_slice *salt, int64_t N, int64_t r, int64_t p,
                             int64_t n, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (N < 2 || r < 1 || p < 1 || n < 1 || n > ((int64_t)1 << 30)) {
        set_err(err, "scrypt: invalid parameters (N a power of two > 1, r >= 1, p >= 1)");
        return;
    }
    uint8_t *o = new_bytes(out, n);
    int rc = slang__scrypt(bdata(pw), (size_t)blen(pw), bdata(salt), (size_t)blen(salt), (uint64_t)N, (uint64_t)r,
                           (uint64_t)p, o, (size_t)n);
    if (rc) {
        slang_slice_release(out);
        set_err(err, rc == 2 ? "scrypt: out of memory" : "scrypt: invalid parameters (N a power of two > 1, r*p < 2^30)");
    }
}

void slang_crypto_hash_blake2b(const slang_slice *data, int64_t size, const slang_slice *key, slang_slice *out,
                               slang_error **err) {
    memset(out, 0, sizeof *out);
    if (size < 1 || size > 64 || blen(key) > 64) {
        set_err(err, "blake2b: size must be 1..64 and the key at most 64 bytes");
        return;
    }
    slang__blake2b(new_bytes(out, size), (size_t)size, bdata(key), (size_t)blen(key), bdata(data), (size_t)blen(data));
}

/* ---- P-384 ---- */

void slang_crypto_ecdh_generate_key_p384(slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    uint8_t *p = new_bytes(out, 48);
    if (!slang__p384_genkey(p)) {
        slang_slice_release(out);
        set_err(err, "crypto/rand: entropy source failed");
    }
}

void slang_crypto_ecdh_public_key_p384(const slang_slice *priv, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(priv) != 48 || !slang__p384_pubkey(new_bytes(out, 97), bdata(priv))) {
        slang_slice_release(out);
        set_err(err, "p384: invalid private key");
    }
}

void slang_crypto_ecdh_shared_secret_p384(const slang_slice *priv, const slang_slice *peer, slang_slice *out,
                                          slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(priv) != 48 || blen(peer) != 97) {
        set_err(err, "p384: invalid key (want a 48-byte private key and a 97-byte uncompressed point)");
        return;
    }
    if (!slang__p384_ecdh(new_bytes(out, 48), bdata(priv), bdata(peer))) {
        slang_slice_release(out);
        set_err(err, "p384: invalid peer public key");
    }
}

void slang_crypto_ecdsa_generate_key_p384(slang_slice *out, slang_error **err) {
    slang_crypto_ecdh_generate_key_p384(out, err);
}

void slang_crypto_ecdsa_public_key_p384(const slang_slice *priv, slang_slice *out, slang_error **err) {
    slang_crypto_ecdh_public_key_p384(priv, out, err);
}

static size_t der_int(uint8_t *o, const uint8_t *v, size_t len) {
    size_t i = 0;
    while (i < len - 1 && v[i] == 0) i++;
    size_t n = len - i, pad = v[i] & 0x80 ? 1 : 0;
    o[0] = 0x02;
    o[1] = (uint8_t)(n + pad);
    if (pad) o[2] = 0;
    memcpy(o + 2 + pad, v + i, n);
    return 2 + pad + n;
}

/* Strict DER ECDSA-Sig-Value -> r||s (len bytes each). */
static bool der_sig_parse(const uint8_t *p, size_t n, uint8_t *rs, size_t len) {
    if (n < 8 || p[0] != 0x30 || p[1] != n - 2 || p[1] & 0x80) return false;
    size_t off = 2;
    memset(rs, 0, 2 * len);
    for (int k = 0; k < 2; k++) {
        if (off + 2 > n || p[off] != 0x02) return false;
        size_t l = p[off + 1];
        off += 2;
        if (l == 0 || l > len + 1 || off + l > n) return false;
        const uint8_t *v = p + off;
        if (v[0] & 0x80) return false;
        if (l > 1 && v[0] == 0 && !(v[1] & 0x80)) return false;
        if (l == len + 1) {
            if (v[0] != 0) return false;
            v++;
            l--;
        }
        memcpy(rs + len * k + (len - l), v, l);
        off += p[off - 1];
    }
    return off == n;
}

void slang_crypto_ecdsa_sign_p384(const slang_slice *priv, const slang_slice *digest, slang_slice *out,
                                  slang_error **err) {
    memset(out, 0, sizeof *out);
    uint8_t sig[96], extra[48];
    if (blen(priv) != 48) {
        set_err(err, "ecdsa: P-384 private key must be 48 bytes");
        return;
    }
    if (!slang__crypto_random(extra, 48) ||
        !slang__p384_sign(sig, bdata(priv), bdata(digest), (size_t)blen(digest), extra, 48)) {
        set_err(err, "ecdsa: signing failed (invalid key)");
        return;
    }
    uint8_t der[110];
    size_t n = der_int(der + 2, sig, 48);
    n += der_int(der + 2 + n, sig + 48, 48);
    der[0] = 0x30;
    der[1] = (uint8_t)n;
    bytes_out(out, der, (int64_t)n + 2);
}

/* Deterministic RFC 6979 signature as raw r||s (no added randomness): for
 * known-answer tests. */
void slang_crypto_ecdsa_sign_p384_raw(const slang_slice *priv, const slang_slice *digest, slang_slice *out,
                                         slang_error **err) {
    memset(out, 0, sizeof *out);
    if (blen(priv) != 48) {
        set_err(err, "ecdsa: P-384 private key must be 48 bytes");
        return;
    }
    if (!slang__p384_sign(new_bytes(out, 96), bdata(priv), bdata(digest), (size_t)blen(digest), NULL, 0)) {
        slang_slice_release(out);
        set_err(err, "ecdsa: signing failed (invalid key)");
    }
}

bool slang_crypto_ecdsa_verify_p384(const slang_slice *pub, const slang_slice *digest, const slang_slice *sig) {
    uint8_t rs[96];
    if (blen(pub) != 97 || !der_sig_parse(bdata(sig), (size_t)blen(sig), rs, 48)) return false;
    return slang__p384_verify(rs, bdata(pub), bdata(digest), (size_t)blen(digest));
}

/* ---- AES-CBC and PKCS#7 ---- */

static bool aes_key(slang_aes_key *k, const slang_slice *key, slang_error **err) {
    if (!slang__aes_init(k, bdata(key), (size_t)blen(key))) {
        set_err(err, "aes: key must be 16, 24 or 32 bytes");
        return false;
    }
    return true;
}

void slang_crypto_aes_decrypt_block(const slang_slice *key, const slang_slice *block, slang_slice *out,
                                    slang_error **err) {
    memset(out, 0, sizeof *out);
    slang_aes_key k;
    if (!aes_key(&k, key, err)) return;
    if (blen(block) != 16) {
        set_err(err, "aes: block must be 16 bytes");
        return;
    }
    slang__aes_decrypt_block(&k, bdata(block), new_bytes(out, 16));
    slang__crypto_wipe(&k, sizeof k);
}

static void cbc(const slang_slice *key, const slang_slice *iv, const slang_slice *in, bool enc, bool pad,
                slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    slang_aes_key k;
    if (!aes_key(&k, key, err)) return;
    if (blen(iv) != 16) {
        set_err(err, "aes-cbc: IV must be 16 bytes");
        return;
    }
    int64_t n = blen(in);
    if (enc) {
        int64_t total = pad ? (n / 16 + 1) * 16 : n;
        if (!pad && n % 16 != 0) {
            set_err(err, "aes-cbc: input is not a multiple of the block size");
            return;
        }
        uint8_t *o = new_bytes(out, total);
        if (total == 0) return;
        memcpy(o, bdata(in), (size_t)n);
        for (int64_t i = n; i < total; i++) o[i] = (uint8_t)(total - n);
        slang__cbc_encrypt(&k, bdata(iv), o, (size_t)total, o);
    } else {
        if (n % 16 != 0 || (pad && n == 0)) {
            set_err(err, "aes-cbc: ciphertext is not a multiple of the block size");
            return;
        }
        uint8_t *o = new_bytes(out, n);
        if (n == 0) return;
        slang__cbc_decrypt(&k, bdata(iv), bdata(in), (size_t)n, o);
        if (pad) {
            size_t pl = slang__pkcs7_unpad_len(o, (size_t)n, 16);
            if (pl == 0) {
                slang__crypto_wipe(o, (size_t)n);
                slang_slice_release(out);
                set_err(err, "aes-cbc: invalid padding");
                return;
            }
            out->len = n - (int64_t)pl;
        }
    }
    slang__crypto_wipe(&k, sizeof k);
}

void slang_crypto_aes_cbc_encrypt(const slang_slice *key, const slang_slice *iv, const slang_slice *pt, slang_slice *out,
                                  slang_error **err) {
    cbc(key, iv, pt, true, true, out, err);
}

void slang_crypto_aes_cbc_decrypt(const slang_slice *key, const slang_slice *iv, const slang_slice *ct, slang_slice *out,
                                  slang_error **err) {
    cbc(key, iv, ct, false, true, out, err);
}

void slang_crypto_aes_cbc_encrypt_raw(const slang_slice *key, const slang_slice *iv, const slang_slice *pt,
                                      slang_slice *out, slang_error **err) {
    cbc(key, iv, pt, true, false, out, err);
}

void slang_crypto_aes_cbc_decrypt_raw(const slang_slice *key, const slang_slice *iv, const slang_slice *ct,
                                      slang_slice *out, slang_error **err) {
    cbc(key, iv, ct, false, false, out, err);
}

void slang_crypto_aes_pkcs7_pad(const slang_slice *data, int64_t block, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (block < 1 || block > 255) {
        set_err(err, "pkcs7: block size must be 1..255");
        return;
    }
    int64_t n = blen(data), total = (n / block + 1) * block;
    uint8_t *o = new_bytes(out, total);
    memcpy(o, bdata(data), (size_t)n);
    for (int64_t i = n; i < total; i++) o[i] = (uint8_t)(total - n);
}

void slang_crypto_aes_pkcs7_unpad(const slang_slice *data, int64_t block, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (block < 1 || block > 255) {
        set_err(err, "pkcs7: block size must be 1..255");
        return;
    }
    size_t pl = slang__pkcs7_unpad_len(bdata(data), (size_t)blen(data), (size_t)block);
    if (pl == 0) {
        set_err(err, "pkcs7: invalid padding");
        return;
    }
    bytes_out(out, bdata(data), blen(data) - (int64_t)pl);
}

/* ---- RSA-OAEP and primes ---- */

void slang_crypto_rsa_encrypt_oaep(const slang_slice *n, const slang_slice *e, const slang_string *hash,
                                   const slang_slice *msg, const slang_slice *label, slang_slice *out,
                                   slang_error **err) {
    memset(out, 0, sizeof *out);
    int a = hash_alg(hash, err);
    if (!a) return;
    uint8_t buf[1024];
    size_t olen = 0;
    if (!slang__rsa_encrypt_oaep(bdata(n), (size_t)blen(n), bdata(e), (size_t)blen(e), a, bdata(msg),
                                 (size_t)blen(msg), bdata(label), (size_t)blen(label), buf, &olen)) {
        set_err(err, "rsa: OAEP encryption failed (invalid key or message too long)");
        return;
    }
    bytes_out(out, buf, (int64_t)olen);
}

void slang_crypto_rsa_decrypt_oaep(const slang_slice *der, const slang_string *hash, const slang_slice *ct,
                                   const slang_slice *label, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    int a = hash_alg(hash, err);
    if (!a) return;
    uint8_t buf[1024];
    size_t mlen = 0;
    if (!slang__rsa_decrypt_oaep(bdata(der), (size_t)blen(der), a, bdata(ct), (size_t)blen(ct), bdata(label),
                                 (size_t)blen(label), buf, &mlen)) {
        set_err(err, "rsa: decryption error");
        return;
    }
    bytes_out(out, buf, (int64_t)mlen);
    slang__crypto_wipe(buf, sizeof buf);
}

void slang_crypto_rsa_rt_gen_prime(int64_t bits, int64_t e, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (bits < 64 || bits > 4096 || bits % 8 != 0) {
        set_err(err, "rsa: prime size must be a multiple of 8 in 64..4096 bits");
        return;
    }
    if (!slang__rsa_gen_prime(new_bytes(out, bits / 8), (int)bits, (uint64_t)e)) {
        slang_slice_release(out);
        set_err(err, "rsa: prime generation failed (entropy source)");
    }
}

bool slang_crypto_rsa_is_probable_prime(const slang_slice *p, int64_t rounds) {
    if (rounds < 1) rounds = 20;
    if (rounds > 128) rounds = 128;
    return slang__rsa_is_probable_prime(bdata(p), (size_t)blen(p), (int)rounds);
}
