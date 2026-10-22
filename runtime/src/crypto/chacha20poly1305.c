/* chacha20poly1305.c - ChaCha20, Poly1305 and the ChaCha20-Poly1305 AEAD
 * (RFC 8439). Constant time by construction (only adds, rotates, xors and
 * 32x32->64 multiplies; Poly1305 uses 26-bit limbs, final reduction by
 * masks). */
#include "crypto.h"

#include <string.h>

#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d)                    \
    a += b; d ^= a; d = ROTL(d, 16);      \
    c += d; b ^= c; b = ROTL(b, 12);      \
    a += b; d ^= a; d = ROTL(d, 8);       \
    c += d; b ^= c; b = ROTL(b, 7)

static void chacha_block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, in, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) zell__st32le(out + 4 * i, x[i] + in[i]);
    zell__crypto_wipe(x, sizeof x);
}

void zell__chacha20(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t *in,
                     size_t n, uint8_t *out) {
    uint32_t st[16];
    st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) st[4 + i] = zell__ld32le(key + 4 * i);
    st[12] = counter;
    for (int i = 0; i < 3; i++) st[13 + i] = zell__ld32le(nonce + 4 * i);
    uint8_t ks[64];
    while (n) {
        chacha_block(st, ks);
        st[12]++;
        size_t m = n < 64 ? n : 64;
        for (size_t i = 0; i < m; i++) out[i] = in[i] ^ ks[i];
        in += m;
        out += m;
        n -= m;
    }
    zell__crypto_wipe(ks, sizeof ks);
    zell__crypto_wipe(st, sizeof st);
}

/* ---- Poly1305 (poly1305-donna style, 5 x 26-bit limbs) ---- */

typedef struct {
    uint32_t r[5], h[5], pad[4];
} poly_state;

static void poly_init(poly_state *p, const uint8_t key[32]) {
    p->r[0] = (zell__ld32le(key + 0)) & 0x3ffffff;
    p->r[1] = (zell__ld32le(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (zell__ld32le(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (zell__ld32le(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (zell__ld32le(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = zell__ld32le(key + 16 + 4 * i);
}

/* Absorbs whole 16-byte blocks; hibit is 1<<24 for full blocks. */
static void poly_blocks(poly_state *p, const uint8_t *m, size_t n, uint32_t hibit) {
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (n >= 16) {
        h0 += (zell__ld32le(m + 0)) & 0x3ffffff;
        h1 += (zell__ld32le(m + 3) >> 2) & 0x3ffffff;
        h2 += (zell__ld32le(m + 6) >> 4) & 0x3ffffff;
        h3 += (zell__ld32le(m + 9) >> 6) & 0x3ffffff;
        h4 += (zell__ld32le(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16;
        n -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

/* Absorbs n bytes; a trailing partial block is padded with 0x01 then zeros
 * (plain Poly1305), or with zeros only as a full block (the AEAD's
 * pad16 framing) when pad16. */
static void poly_update(poly_state *p, const uint8_t *m, size_t n, bool pad16) {
    size_t full = n & ~(size_t)15;
    poly_blocks(p, m, full, 1u << 24);
    if (n > full) {
        uint8_t blk[16] = { 0 };
        memcpy(blk, m + full, n - full);
        if (pad16) {
            poly_blocks(p, blk, 16, 1u << 24);
        } else {
            blk[n - full] = 1;
            poly_blocks(p, blk, 16, 0);
        }
    }
}

static void poly_finish(poly_state *p, uint8_t tag[16]) {
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    /* g = h + 5 - 2^130 */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;   /* all ones if h >= p */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0 | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;
    uint64_t f;
    f = (uint64_t)h0 + p->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + p->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p->pad[3] + (f >> 32); h3 = (uint32_t)f;
    zell__st32le(tag, h0);
    zell__st32le(tag + 4, h1);
    zell__st32le(tag + 8, h2);
    zell__st32le(tag + 12, h3);
    zell__crypto_wipe(p, sizeof *p);
}

void zell__poly1305(const uint8_t key[32], const uint8_t *msg, size_t n, uint8_t tag[16]) {
    poly_state p;
    poly_init(&p, key);
    poly_update(&p, msg, n, false);
    poly_finish(&p, tag);
}

/* ---- AEAD (RFC 8439 2.8) ---- */

static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                     const uint8_t *ct, size_t n, uint8_t tag[16]) {
    uint8_t otk[64] = { 0 };
    zell__chacha20(key, nonce, 0, otk, 64, otk);
    poly_state p;
    poly_init(&p, otk);
    poly_update(&p, aad, alen, true);
    poly_update(&p, ct, n, true);
    uint8_t lens[16];
    zell__st64le(lens, (uint64_t)alen);
    zell__st64le(lens + 8, (uint64_t)n);
    poly_blocks(&p, lens, 16, 1u << 24);
    poly_finish(&p, tag);
    zell__crypto_wipe(otk, sizeof otk);
}

void zell__chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                                  const uint8_t *in, size_t n, uint8_t *out) {
    zell__chacha20(key, nonce, 1, in, n, out);
    aead_tag(key, nonce, aad, alen, out, n, out + n);
}

bool zell__chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t alen,
                                  const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t tag[16];
    aead_tag(key, nonce, aad, alen, in, n, tag);
    if (!zell__crypto_ct_eq(tag, in + n, 16)) {
        memset(out, 0, n);
        return false;
    }
    zell__chacha20(key, nonce, 1, in, n, out);
    return true;
}
