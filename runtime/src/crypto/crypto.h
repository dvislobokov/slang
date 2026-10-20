/* crypto.h - internal API of the cryptographic primitives (runtime/src/crypto/).
 *
 * Everything here is private to the runtime (slang__ prefix). The Slang-facing
 * wrappers (std.crypto.aes/chacha/x25519/ecdsa/ed25519/rsa/hkdf and the
 * internal rt* helpers of std.net.tls / std.crypto.x509) are in
 * api.c; see runtime/ABI.md section 19.
 *
 * Rules for every primitive:
 *   - no secret-dependent branches or memory indices (constant time), except
 *     where the operation only handles public data (signature verification,
 *     RSA public operations, parsing);
 *   - no heap allocation except where noted, no global mutable state except
 *     the once-initialized CPU feature flags;
 *   - secrets on the stack are wiped with slang__crypto_wipe before return.
 */
#ifndef SLANG_CRYPTO_H
#define SLANG_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLANG_CRYPTO_HIDDEN __attribute__((visibility("hidden")))

/* ---- utilities (util.c) ---- */

SLANG_CRYPTO_HIDDEN void slang__crypto_wipe(void *p, size_t n);
/* 1 if equal, 0 otherwise; time depends only on n. */
SLANG_CRYPTO_HIDDEN int slang__crypto_ct_eq(const uint8_t *a, const uint8_t *b, size_t n);
SLANG_CRYPTO_HIDDEN bool slang__crypto_random(uint8_t *buf, size_t n);
/* CPU features (x86-64 cpuid; always false elsewhere). SLANG_NO_AESNI=1 in
 * the environment disables the AES-NI/PCLMULQDQ path (for tests/benchmarks). */
SLANG_CRYPTO_HIDDEN bool slang__cpu_has_aesni(void);
/* Forces the portable AES path on (1) or back to auto-detection (0). */
SLANG_CRYPTO_HIDDEN void slang__aes_force_portable(int on);

static inline uint32_t slang__ld32be(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint64_t slang__ld64be(const uint8_t *p) {
    return (uint64_t)slang__ld32be(p) << 32 | slang__ld32be(p + 4);
}
static inline void slang__st32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void slang__st64be(uint8_t *p, uint64_t v) {
    slang__st32be(p, (uint32_t)(v >> 32)); slang__st32be(p + 4, (uint32_t)v);
}
static inline uint32_t slang__ld32le(const uint8_t *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static inline void slang__st32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint64_t slang__ld64le(const uint8_t *p) {
    return (uint64_t)slang__ld32le(p) | (uint64_t)slang__ld32le(p + 4) << 32;
}
static inline void slang__st64le(uint8_t *p, uint64_t v) {
    slang__st32le(p, (uint32_t)v); slang__st32le(p + 4, (uint32_t)(v >> 32));
}

/* ---- SHA-2 (sha2.c, FIPS 180-4) and HMAC (RFC 2104) ---- */

typedef enum { SLANG_HASH_SHA256 = 1, SLANG_HASH_SHA384 = 2, SLANG_HASH_SHA512 = 3, SLANG_HASH_SHA1 = 4 } slang_hash_alg;

typedef struct {
    uint32_t alg;          /* slang_hash_alg */
    uint32_t buflen;       /* bytes in buf */
    uint64_t total;        /* bytes hashed so far */
    union { uint32_t h32[8]; uint64_t h64[8]; } h;
    uint8_t buf[128];
} slang_hash_ctx;          /* plain bytes: may be copied and stored anywhere */

/* Digest size (0 for an unknown algorithm) and block size. */
SLANG_CRYPTO_HIDDEN size_t slang__hash_size(int alg);
SLANG_CRYPTO_HIDDEN size_t slang__hash_block(int alg);
SLANG_CRYPTO_HIDDEN bool slang__hash_init(slang_hash_ctx *c, int alg);
SLANG_CRYPTO_HIDDEN void slang__hash_update(slang_hash_ctx *c, const uint8_t *p, size_t n);
/* Writes the digest; c is left finalized (copy it first to continue). */
SLANG_CRYPTO_HIDDEN void slang__hash_final(slang_hash_ctx *c, uint8_t *out);
SLANG_CRYPTO_HIDDEN void slang__hash(int alg, const uint8_t *p, size_t n, uint8_t *out);
/* Algorithm by name: "sha256", "sha384", "sha512", "sha1" (also "SHA-256"...); 0 if unknown. */
SLANG_CRYPTO_HIDDEN int slang__hash_by_name(const char *name, size_t len);

typedef struct {
    slang_hash_ctx inner, outer;
} slang_hmac_ctx;

SLANG_CRYPTO_HIDDEN void slang__hmac_init(slang_hmac_ctx *c, int alg, const uint8_t *key, size_t klen);
SLANG_CRYPTO_HIDDEN void slang__hmac_update(slang_hmac_ctx *c, const uint8_t *p, size_t n);
SLANG_CRYPTO_HIDDEN void slang__hmac_final(slang_hmac_ctx *c, uint8_t *out);
SLANG_CRYPTO_HIDDEN void slang__hmac(int alg, const uint8_t *key, size_t klen, const uint8_t *msg, size_t n, uint8_t *out);

/* ---- HKDF (hkdf.c, RFC 5869) ---- */

/* prk has slang__hash_size(alg) bytes. */
SLANG_CRYPTO_HIDDEN void slang__hkdf_extract(int alg, const uint8_t *salt, size_t slen,
                                             const uint8_t *ikm, size_t ilen, uint8_t *prk);
/* false if n > 255 * hash size. */
SLANG_CRYPTO_HIDDEN bool slang__hkdf_expand(int alg, const uint8_t *prk, size_t plen,
                                            const uint8_t *info, size_t ilen, uint8_t *out, size_t n);

/* ---- AES and AES-GCM (aes.c, FIPS 197, SP 800-38D) ---- */

typedef struct {
    uint32_t nr;           /* 10 or 14 rounds */
    uint32_t ni;           /* 1: round keys are for the AES-NI path */
    uint8_t rk[15 * 16];   /* raw round keys (encryption) */
    uint64_t sk[15 * 8];   /* bitsliced round keys (portable path) */
} slang_aes_key;

typedef struct {
    slang_aes_key k;
    uint8_t h[16];         /* GHASH key H = E(K, 0^128) */
    uint64_t h0, h1;       /* H as big-endian halves (portable GHASH) */
} slang_gcm_ctx;

/* klen 16 or 32 (24 accepted too). */
SLANG_CRYPTO_HIDDEN bool slang__aes_init(slang_aes_key *k, const uint8_t *key, size_t klen);
SLANG_CRYPTO_HIDDEN void slang__aes_encrypt_block(const slang_aes_key *k, const uint8_t in[16], uint8_t out[16]);
SLANG_CRYPTO_HIDDEN bool slang__gcm_init(slang_gcm_ctx *c, const uint8_t *key, size_t klen);
/* 12-byte nonce; out gets n bytes of ciphertext followed by the 16-byte tag. */
SLANG_CRYPTO_HIDDEN void slang__gcm_seal(const slang_gcm_ctx *c, const uint8_t nonce[12],
                                         const uint8_t *aad, size_t alen,
                                         const uint8_t *in, size_t n, uint8_t *out);
/* in has n bytes of ciphertext + 16-byte tag; out gets n bytes; false on a bad tag
 * (out is zeroed then). */
SLANG_CRYPTO_HIDDEN bool slang__gcm_open(const slang_gcm_ctx *c, const uint8_t nonce[12],
                                         const uint8_t *aad, size_t alen,
                                         const uint8_t *in, size_t n, uint8_t *out);
/* The bitsliced S-box applied to one byte (for tests). */
SLANG_CRYPTO_HIDDEN uint8_t slang__aes_sbox_ct(uint8_t x);

/* ---- ChaCha20-Poly1305 (chacha20poly1305.c, RFC 8439) ---- */

SLANG_CRYPTO_HIDDEN void slang__chacha20(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                                         const uint8_t *in, size_t n, uint8_t *out);
SLANG_CRYPTO_HIDDEN void slang__poly1305(const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t tag[16]);
SLANG_CRYPTO_HIDDEN void slang__chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                                                      const uint8_t *aad, size_t alen,
                                                      const uint8_t *in, size_t n, uint8_t *out);
SLANG_CRYPTO_HIDDEN bool slang__chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                                                      const uint8_t *aad, size_t alen,
                                                      const uint8_t *in, size_t n, uint8_t *out);

/* ---- X25519 (x25519.c, RFC 7748) ---- */

/* false if the result is all zero (small-order peer point). */
SLANG_CRYPTO_HIDDEN bool slang__x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
SLANG_CRYPTO_HIDDEN void slang__x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* ---- Ed25519 (ed25519.c, RFC 8032) ---- */

SLANG_CRYPTO_HIDDEN void slang__ed25519_pubkey(uint8_t pub[32], const uint8_t seed[32]);
SLANG_CRYPTO_HIDDEN void slang__ed25519_sign(uint8_t sig[64], const uint8_t seed[32],
                                             const uint8_t *msg, size_t n);
SLANG_CRYPTO_HIDDEN bool slang__ed25519_verify(const uint8_t sig[64], const uint8_t pub[32],
                                               const uint8_t *msg, size_t n);

/* ---- P-256 (p256.c, FIPS 186-4, SEC 1, RFC 6979) ---- */

/* pub: 65-byte uncompressed point 04||X||Y. false for an invalid scalar (0 or >= n). */
SLANG_CRYPTO_HIDDEN bool slang__p256_pubkey(uint8_t pub[65], const uint8_t priv[32]);
/* ECDH: x coordinate of priv * peer; false if peer is not on the curve. */
SLANG_CRYPTO_HIDDEN bool slang__p256_ecdh(uint8_t out[32], const uint8_t priv[32], const uint8_t peer[65]);
/* ECDSA over a message digest (leftmost 256 bits used); deterministic nonce
 * (RFC 6979 with HMAC-SHA256) mixed with `extra` random bytes when extra != NULL.
 * sig = r||s (64 bytes). */
SLANG_CRYPTO_HIDDEN bool slang__p256_sign(uint8_t sig[64], const uint8_t priv[32], const uint8_t *digest,
                                          size_t dlen, const uint8_t *extra, size_t elen);
SLANG_CRYPTO_HIDDEN bool slang__p256_verify(const uint8_t sig[64], const uint8_t pub[65],
                                            const uint8_t *digest, size_t dlen);
/* Generates a random valid private key. */
SLANG_CRYPTO_HIDDEN bool slang__p256_genkey(uint8_t priv[32]);

/* ---- RSA (rsa.c, RFC 8017) ---- */

/* Public key: modulus n (big-endian, nlen bytes, 1024..8192 bits) and exponent e. */
/* Verifies an RSASSA-PKCS1-v1_5 signature over a digest of hash `alg`. */
SLANG_CRYPTO_HIDDEN bool slang__rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                                                 int alg, const uint8_t *digest, size_t dlen,
                                                 const uint8_t *sig, size_t slen);
/* Verifies RSASSA-PSS (MGF1 with the same hash, salt length = hash length or any when
 * saltlen < 0). */
SLANG_CRYPTO_HIDDEN bool slang__rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                                               int alg, const uint8_t *digest, size_t dlen,
                                               const uint8_t *sig, size_t slen, int saltlen);
/* Private-key operations take a PKCS#1 RSAPrivateKey DER. sig gets the modulus
 * size in bytes (*siglen). Exponent blinding + result verification. */
SLANG_CRYPTO_HIDDEN bool slang__rsa_sign_pkcs1(const uint8_t *der, size_t derlen, int alg,
                                               const uint8_t *digest, size_t dlen, uint8_t *sig, size_t *siglen);
SLANG_CRYPTO_HIDDEN bool slang__rsa_sign_pss(const uint8_t *der, size_t derlen, int alg,
                                             const uint8_t *digest, size_t dlen, uint8_t *sig, size_t *siglen);
/* Modulus size in bytes of a PKCS#1 private key (0 if malformed). */
SLANG_CRYPTO_HIDDEN size_t slang__rsa_priv_size(const uint8_t *der, size_t derlen);
/* Raw public operation sig^e mod n into out (nlen bytes), for tests. */
SLANG_CRYPTO_HIDDEN bool slang__rsa_public(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen,
                                           const uint8_t *in, size_t ilen, uint8_t *out);

/* ---- P-384 (p384.c, same contract as P-256 with 48-byte scalars,
 * 97-byte points and HMAC-SHA384 RFC 6979 nonces) ---- */

SLANG_CRYPTO_HIDDEN bool slang__p384_pubkey(uint8_t pub[97], const uint8_t priv[48]);
SLANG_CRYPTO_HIDDEN bool slang__p384_ecdh(uint8_t out[48], const uint8_t priv[48], const uint8_t peer[97]);
SLANG_CRYPTO_HIDDEN bool slang__p384_sign(uint8_t sig[96], const uint8_t priv[48], const uint8_t *digest,
                                          size_t dlen, const uint8_t *extra, size_t elen);
SLANG_CRYPTO_HIDDEN bool slang__p384_verify(const uint8_t sig[96], const uint8_t pub[97],
                                            const uint8_t *digest, size_t dlen);
SLANG_CRYPTO_HIDDEN bool slang__p384_genkey(uint8_t priv[48]);

/* ---- AES decryption, CBC and PKCS#7 (cbc.c) ---- */

SLANG_CRYPTO_HIDDEN void slang__aes_decrypt_block(const slang_aes_key *k, const uint8_t in[16], uint8_t out[16]);
/* n is a multiple of 16; in and out may be equal. */
SLANG_CRYPTO_HIDDEN void slang__cbc_encrypt(const slang_aes_key *k, const uint8_t iv[16], const uint8_t *in,
                                            size_t n, uint8_t *out);
SLANG_CRYPTO_HIDDEN void slang__cbc_decrypt(const slang_aes_key *k, const uint8_t iv[16], const uint8_t *in,
                                            size_t n, uint8_t *out);
/* Padding length (1..block) of a padded buffer, 0 if invalid (constant time). */
SLANG_CRYPTO_HIDDEN size_t slang__pkcs7_unpad_len(const uint8_t *p, size_t n, size_t block);

/* ---- password-based KDFs (kdf.c, ../std_argon2.c) ---- */

SLANG_CRYPTO_HIDDEN bool slang__pbkdf2(int alg, const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen,
                                       uint64_t iter, uint8_t *out, size_t n);
/* 0 ok, 1 bad parameters (N a power of two > 1, r p < 2^30), 2 out of memory. */
SLANG_CRYPTO_HIDDEN int slang__scrypt(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen, uint64_t N,
                                      uint64_t r, uint64_t p, uint8_t *out, size_t n);
SLANG_CRYPTO_HIDDEN void slang__blake2b(uint8_t *out, size_t outlen, const uint8_t *key, size_t klen,
                                        const uint8_t *p, size_t n);
/* type 0 Argon2d, 1 Argon2i, 2 Argon2id; 0 ok, 1 bad parameters, 2 out of memory. */
SLANG_CRYPTO_HIDDEN int slang__argon2(int type, const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen,
                                      const uint8_t *secret, size_t klen, const uint8_t *ad, size_t adlen, uint32_t t,
                                      uint32_t m, uint32_t p, uint8_t *out, size_t T);

/* ---- RSA additions (rsa.c): OAEP (RFC 8017 7.1) and prime generation ---- */

/* out gets the modulus size in bytes (*olen). */
SLANG_CRYPTO_HIDDEN bool slang__rsa_encrypt_oaep(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int alg,
                                                 const uint8_t *msg, size_t mlen, const uint8_t *label, size_t llen,
                                                 uint8_t *out, size_t *olen);
/* Decrypts into out (at least modulus size bytes), *mlen = message length;
 * false on any failure (no distinction, constant time decoding). */
SLANG_CRYPTO_HIDDEN bool slang__rsa_decrypt_oaep(const uint8_t *der, size_t derlen, int alg, const uint8_t *ct,
                                                 size_t clen, const uint8_t *label, size_t llen, uint8_t *out,
                                                 size_t *mlen);
/* A random prime of exactly `bits` bits (top two bits set) with p mod e != 1;
 * big-endian into out (bits/8 bytes). */
SLANG_CRYPTO_HIDDEN bool slang__rsa_gen_prime(uint8_t *out, int bits, uint64_t e);
/* Miller-Rabin with `rounds` random bases (public data, variable time). */
SLANG_CRYPTO_HIDDEN bool slang__rsa_is_probable_prime(const uint8_t *p, size_t plen, int rounds);

#endif
