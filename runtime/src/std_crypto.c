/* std_crypto.c - std.crypto.hash (SHA-1, SHA-256, SHA-512, MD5, CRC-32),
 * std.crypto.hmac (HMAC-SHA256) and std.crypto.rand (getrandom(2), with a
 * /dev/urandom fallback). Implemented from FIPS 180-4, RFC 1321, RFC 2104 and
 * the IEEE 802.3 CRC-32 polynomial; no external libraries. Inputs are the raw
 * bytes of the Slang string. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static inline int64_t slen(const slang_string *s) { return s ? s->len : 0; }
static inline const uint8_t *sbytes(const slang_string *s) {
    return (const uint8_t *)(s ? s->data : "");
}

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
static inline uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* Merkle-Damgard driver: feeds whole blocks of msg + padding to `block`.
 * Length field is big-endian unless `le_len`; `lenbytes` is 8 or 16. */
typedef void (*block_fn)(void *state, const uint8_t *blk);
static void md_hash(void *state, block_fn block, size_t bs, size_t lenbytes, bool le_len,
                    const uint8_t *msg, uint64_t n) {
    uint64_t i = 0;
    for (; i + bs <= n; i += bs) block(state, msg + i);
    uint8_t tail[256];
    size_t rem = (size_t)(n - i);
    memset(tail, 0, sizeof tail);
    if (rem) memcpy(tail, msg + i, rem);
    tail[rem] = 0x80;
    size_t total = rem + 1 + lenbytes <= bs ? bs : 2 * bs;
    uint64_t bits = n * 8;   /* messages < 2^61 bytes: high length bits are 0 */
    for (int k = 0; k < 8; k++) {
        uint8_t b = (uint8_t)(bits >> (8 * k));
        if (le_len) tail[total - lenbytes + (size_t)k] = b;
        else tail[total - 1 - (size_t)k] = b;
    }
    if (le_len) for (size_t k = 8; k < lenbytes; k++) tail[total - lenbytes + k] = 0;
    block(state, tail);
    if (total == 2 * bs) block(state, tail + bs);
}

/* ---- SHA-256 (FIPS 180-4 6.2) ---- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(void *st, const uint8_t *p) {
    uint32_t *h = st, w[64];
    for (int t = 0; t < 16; t++) w[t] = be32(p + 4 * t);
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = rotr32(w[t - 15], 7) ^ rotr32(w[t - 15], 18) ^ (w[t - 15] >> 3);
        uint32_t s1 = rotr32(w[t - 2], 17) ^ rotr32(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 64; t++) {
        uint32_t t1 = hh + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) + ((e & f) ^ (~e & g)) + K256[t] + w[t];
        uint32_t t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256(const uint8_t *msg, uint64_t n, uint8_t out[32]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    md_hash(h, sha256_block, 64, 8, false, msg, n);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(h[i] >> (24 - 8 * k));
}

/* ---- SHA-512 (FIPS 180-4 6.4) ---- */

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static void sha512_block(void *st, const uint8_t *p) {
    uint64_t *h = st, w[80];
    for (int t = 0; t < 16; t++) w[t] = be64(p + 8 * t);
    for (int t = 16; t < 80; t++) {
        uint64_t s0 = rotr64(w[t - 15], 1) ^ rotr64(w[t - 15], 8) ^ (w[t - 15] >> 7);
        uint64_t s1 = rotr64(w[t - 2], 19) ^ rotr64(w[t - 2], 61) ^ (w[t - 2] >> 6);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int t = 0; t < 80; t++) {
        uint64_t t1 = hh + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) + K512[t] + w[t];
        uint64_t t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha512(const uint8_t *msg, uint64_t n, uint8_t out[64]) {
    uint64_t h[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                      0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                      0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    md_hash(h, sha512_block, 128, 16, false, msg, n);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 8; k++) out[8 * i + k] = (uint8_t)(h[i] >> (56 - 8 * k));
}

/* ---- SHA-1 (FIPS 180-4 6.1) ---- */

static void sha1_block(void *st, const uint8_t *p) {
    uint32_t *h = st, w[80];
    for (int t = 0; t < 16; t++) w[t] = be32(p + 4 * t);
    for (int t = 16; t < 80; t++) w[t] = rotl32(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; t++) {
        uint32_t f, k;
        if (t < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (t < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t tmp = rotl32(a, 5) + f + e + k + w[t];
        e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const uint8_t *msg, uint64_t n, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    md_hash(h, sha1_block, 64, 8, false, msg, n);
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(h[i] >> (24 - 8 * k));
}

/* ---- MD5 (RFC 1321) ---- */

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const int MD5_S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(void *st, const uint8_t *p) {
    uint32_t *h = st, m[16];
    for (int i = 0; i < 16; i++) m[i] = le32(p + 4 * i);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + MD5_K[i] + m[g], MD5_S[i]);
        a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

static void md5(const uint8_t *msg, uint64_t n, uint8_t out[16]) {
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    md_hash(h, md5_block, 64, 8, true, msg, n);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(h[i] >> (8 * k));
}

/* ---- CRC-32 (IEEE 802.3, reflected 0xEDB88320, as zlib / Go crc32.ChecksumIEEE) ---- */

static uint32_t crc32_ieee(const uint8_t *p, uint64_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint64_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* ---- result helpers ---- */

static void bytes_out(slang_slice *out, const uint8_t *d, int64_t n) {
    slang_slice_from(out, 1, NULL, d, n);
}

static slang_string *hex_str(const uint8_t *d, int64_t n) {
    static const char hx[] = "0123456789abcdef";
    slang_string *r = slang__string_alloc(2 * n);
    for (int64_t i = 0; i < n; i++) {
        r->data[2 * i] = hx[d[i] >> 4];
        r->data[2 * i + 1] = hx[d[i] & 15];
    }
    return r;
}

#define HASH_FUNCS(name, fn, size)                                                       \
    void slang_crypto_hash_##name(const slang_string *data, slang_slice *out) {         \
        uint8_t d[size];                                                                 \
        fn(sbytes(data), (uint64_t)slen(data), d);                                       \
        bytes_out(out, d, size);                                                         \
    }                                                                                    \
    slang_string *slang_crypto_hash_##name##_hex(const slang_string *data) {            \
        uint8_t d[size];                                                                 \
        fn(sbytes(data), (uint64_t)slen(data), d);                                       \
        return hex_str(d, size);                                                         \
    }

HASH_FUNCS(sha256, sha256, 32)
HASH_FUNCS(sha512, sha512, 64)
HASH_FUNCS(sha1, sha1, 20)
HASH_FUNCS(md5, md5, 16)

int64_t slang_crypto_hash_crc32(const slang_string *data) {
    return (int64_t)crc32_ieee(sbytes(data), (uint64_t)slen(data));
}

/* ---- HMAC-SHA256 (RFC 2104, B = 64) ---- */

static void hmac_sha256(const slang_string *key, const slang_string *data, uint8_t out[32]) {
    uint8_t k[64] = { 0 }, inner[32];
    if (slen(key) > 64) sha256(sbytes(key), (uint64_t)slen(key), k);
    else if (slen(key)) memcpy(k, sbytes(key), (size_t)slen(key));
    int64_t n = slen(data);
    uint8_t *buf = slang_alloc(64 + (n > 32 ? n : 32));
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x36;
    if (n) memcpy(buf + 64, sbytes(data), (size_t)n);
    sha256(buf, (uint64_t)(64 + n), inner);
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x5c;
    memcpy(buf + 64, inner, 32);
    sha256(buf, 96, out);
    slang_free(buf);
}

void slang_crypto_hmac_sha256(const slang_string *key, const slang_string *data, slang_slice *out) {
    uint8_t d[32];
    hmac_sha256(key, data, d);
    bytes_out(out, d, 32);
}

slang_string *slang_crypto_hmac_sha256_hex(const slang_string *key, const slang_string *data) {
    uint8_t d[32];
    hmac_sha256(key, data, d);
    return hex_str(d, 32);
}

/* Constant-time (in the contents) comparison of two MACs. */
bool slang_crypto_hmac_equal(const slang_slice *a, const slang_slice *b) {
    int64_t na = a ? a->len : 0, nb = b ? b->len : 0;
    if (na != nb) return false;
    const uint8_t *pa = na ? a->data : NULL, *pb = nb ? b->data : NULL;
    uint8_t acc = 0;
    for (int64_t i = 0; i < na; i++) acc |= (uint8_t)(pa[i] ^ pb[i]);
    return acc == 0;
}

/* ---- std.crypto.rand ---- */

/* Fills buf with n random bytes; returns 0 or an errno value. */
static int fill_random(uint8_t *buf, size_t n) {
    return slang__plat_random_bytes(buf, n) ? 0 : (errno ? errno : EIO);
}

static void rand_err(slang_error **err, int e) {
    if (!err) return;
    char buf[160];
    snprintf(buf, sizeof buf, "crypto/rand: %s", strerror(e));
    *err = slang_error_new_cstr(buf);
}

static bool check_n(int64_t n, slang_error **err) {
    if (n >= 0) return true;
    if (err) *err = slang_error_new_cstr("crypto/rand: negative length");
    return false;
}

void slang_crypto_rand_bytes(int64_t n, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (!check_n(n, err)) return;
    slang_slice_new(out, 1, n, n);
    int e = n ? fill_random(out->data, (size_t)n) : 0;
    if (e) { slang_slice_release(out); rand_err(err, e); }
}

int64_t slang_crypto_rand_int(slang_error **err) {
    uint64_t v = 0;
    int e = fill_random((uint8_t *)&v, sizeof v);
    if (e) { rand_err(err, e); return 0; }
    return (int64_t)(v >> 1);   /* uniform in [0, 2^63) */
}

slang_string *slang_crypto_rand_hex(int64_t n, slang_error **err) {
    if (!check_n(n, err)) return NULL;
    uint8_t *b = slang_alloc(n ? n : 1);
    int e = n ? fill_random(b, (size_t)n) : 0;
    if (e) { slang_free(b); rand_err(err, e); return NULL; }
    slang_string *r = hex_str(b, n);
    slang_free(b);
    return r;
}
