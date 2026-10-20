/* p384.c - NIST P-384 (secp384r1): ECDH (SEC 1 3.3.1) and ECDSA
 * (FIPS 186-4 6, deterministic nonces per RFC 6979 3.2 with HMAC-SHA384,
 * optionally hedged with extra randomness as in RFC 6979 3.6).
 *
 * Same design as p256.c, with 6 x 64-bit limbs: Montgomery multiplication
 * (CIOS) modulo p or n with constant-time final subtraction, masked
 * additions/subtractions, projective points with the complete addition
 * formula of Renes-Costello-Batina 2016 (algorithm 4, a = -3), and a 4-bit
 * fixed-window scalar multiplication with a constant-time table scan. */
#include "crypto.h"

#include <string.h>

#define NL 6
#define NB 48

typedef unsigned __int128 u128;
typedef uint64_t limb6[NL];

typedef struct {
    limb6 m;        /* modulus */
    uint64_t m0;    /* -m^-1 mod 2^64 */
    limb6 rr;       /* R^2 mod m, R = 2^384 */
    limb6 one;      /* R mod m */
} mont;

static const limb6 P_M = { 0x00000000ffffffffULL, 0xffffffff00000000ULL, 0xfffffffffffffffeULL,
                           0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL };
static const limb6 N_M = { 0xecec196accc52973ULL, 0x581a0db248b0a77aULL, 0xc7634d81f4372ddfULL,
                           0xffffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL };
static const uint8_t B_BYTES[NB] = {
    0xb3, 0x31, 0x2f, 0xa7, 0xe2, 0x3e, 0xe7, 0xe4, 0x98, 0x8e, 0x05, 0x6b, 0xe3, 0xf8, 0x2d, 0x19,
    0x18, 0x1d, 0x9c, 0x6e, 0xfe, 0x81, 0x41, 0x12, 0x03, 0x14, 0x08, 0x8f, 0x50, 0x13, 0x87, 0x5a,
    0xc6, 0x56, 0x39, 0x8d, 0x8a, 0x2e, 0xd1, 0x9d, 0x2a, 0x85, 0xc8, 0xed, 0xd3, 0xec, 0x2a, 0xef,
};
static const uint8_t GX_BYTES[NB] = {
    0xaa, 0x87, 0xca, 0x22, 0xbe, 0x8b, 0x05, 0x37, 0x8e, 0xb1, 0xc7, 0x1e, 0xf3, 0x20, 0xad, 0x74,
    0x6e, 0x1d, 0x3b, 0x62, 0x8b, 0xa7, 0x9b, 0x98, 0x59, 0xf7, 0x41, 0xe0, 0x82, 0x54, 0x2a, 0x38,
    0x55, 0x02, 0xf2, 0x5d, 0xbf, 0x55, 0x29, 0x6c, 0x3a, 0x54, 0x5e, 0x38, 0x72, 0x76, 0x0a, 0xb7,
};
static const uint8_t GY_BYTES[NB] = {
    0x36, 0x17, 0xde, 0x4a, 0x96, 0x26, 0x2c, 0x6f, 0x5d, 0x9e, 0x98, 0xbf, 0x92, 0x92, 0xdc, 0x29,
    0xf8, 0xf4, 0x1d, 0xbd, 0x28, 0x9a, 0x14, 0x7c, 0xe9, 0xda, 0x31, 0x13, 0xb5, 0xf0, 0xb8, 0xc0,
    0x0a, 0x60, 0xb1, 0xce, 0x1d, 0x7e, 0x81, 0x9d, 0x7a, 0x43, 0x1d, 0x7c, 0x90, 0xea, 0x0e, 0x5f,
};

static void l6_from_be(limb6 r, const uint8_t b[NB]) {
    for (int i = 0; i < NL; i++) r[i] = slang__ld64be(b + 8 * (NL - 1) - 8 * i);
}
static void l6_to_be(uint8_t b[NB], const limb6 a) {
    for (int i = 0; i < NL; i++) slang__st64be(b + 8 * (NL - 1) - 8 * i, a[i]);
}
static void l6_copy(limb6 r, const limb6 a) { for (int i = 0; i < NL; i++) r[i] = a[i]; }

static uint64_t l6_sub(limb6 r, const limb6 a, const limb6 b) {
    uint64_t borrow = 0;
    for (int i = 0; i < NL; i++) {
        u128 t = (u128)a[i] - b[i] - borrow;
        r[i] = (uint64_t)t;
        borrow = (uint64_t)(t >> 64) & 1;
    }
    return borrow;
}
static uint64_t l6_add(limb6 r, const limb6 a, const limb6 b) {
    uint64_t carry = 0;
    for (int i = 0; i < NL; i++) {
        u128 t = (u128)a[i] + b[i] + carry;
        r[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
    return carry;
}
static void l6_cmov(limb6 r, const limb6 a, uint64_t mask) {
    for (int i = 0; i < NL; i++) r[i] ^= (r[i] ^ a[i]) & mask;
}
static uint64_t l6_is_zero(const limb6 a) {
    uint64_t x = 0;
    for (int i = 0; i < NL; i++) x |= a[i];
    return 1 & ~((x | (0 - x)) >> 63);
}
static uint64_t l6_lt(const limb6 a, const limb6 m) {
    limb6 t;
    return l6_sub(t, a, m);
}

static void mont_init(mont *c, const limb6 m) {
    l6_copy(c->m, m);
    uint64_t inv = 1;
    for (int i = 0; i < 7; i++) inv *= 2 - m[0] * inv;
    c->m0 = 0 - inv;
    limb6 z = { 0 };
    l6_sub(c->one, z, m);   /* 2^384 - m (m > 2^383) */
    limb6 x, t;
    l6_copy(x, c->one);
    for (int i = 0; i < 64 * NL; i++) {
        uint64_t carry = l6_add(x, x, x);
        uint64_t borrow = l6_sub(t, x, m);
        l6_cmov(x, t, 0 - (carry | (borrow ^ 1)));
    }
    l6_copy(c->rr, x);
}

static void mont_mul(const mont *c, limb6 r, const limb6 a, const limb6 b) {
    uint64_t t[NL + 2] = { 0 };
    for (int i = 0; i < NL; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < NL; j++) {
            u128 x = (u128)a[j] * b[i] + t[j] + carry;
            t[j] = (uint64_t)x;
            carry = (uint64_t)(x >> 64);
        }
        u128 x = (u128)t[NL] + carry;
        t[NL] = (uint64_t)x;
        t[NL + 1] = (uint64_t)(x >> 64);
        uint64_t mm = t[0] * c->m0;
        x = (u128)mm * c->m[0] + t[0];
        carry = (uint64_t)(x >> 64);
        for (int j = 1; j < NL; j++) {
            x = (u128)mm * c->m[j] + t[j] + carry;
            t[j - 1] = (uint64_t)x;
            carry = (uint64_t)(x >> 64);
        }
        x = (u128)t[NL] + carry;
        t[NL - 1] = (uint64_t)x;
        t[NL] = t[NL + 1] + (uint64_t)(x >> 64);
    }
    limb6 res, d;
    for (int i = 0; i < NL; i++) res[i] = t[i];
    uint64_t borrow = l6_sub(d, res, c->m);
    l6_cmov(res, d, 0 - (t[NL] | (borrow ^ 1)));
    l6_copy(r, res);
}

static void mont_add(const mont *c, limb6 r, const limb6 a, const limb6 b) {
    limb6 s, d;
    uint64_t carry = l6_add(s, a, b);
    uint64_t borrow = l6_sub(d, s, c->m);
    l6_cmov(s, d, 0 - (carry | (borrow ^ 1)));
    l6_copy(r, s);
}

static void mont_sub(const mont *c, limb6 r, const limb6 a, const limb6 b) {
    limb6 d, e;
    uint64_t borrow = l6_sub(d, a, b);
    l6_add(e, d, c->m);
    l6_cmov(d, e, 0 - borrow);
    l6_copy(r, d);
}

static void mont_to(const mont *c, limb6 r, const limb6 a) { mont_mul(c, r, a, c->rr); }
static void mont_from(const mont *c, limb6 r, const limb6 a) {
    limb6 one = { 1 };
    mont_mul(c, r, a, one);
}

/* r = a^(m-2); the exponent is public. */
static void mont_inv(const mont *c, limb6 r, const limb6 a) {
    limb6 e, two = { 2 }, acc;
    l6_sub(e, c->m, two);
    l6_copy(acc, c->one);
    for (int i = 64 * NL - 1; i >= 0; i--) {
        mont_mul(c, acc, acc, acc);
        if ((e[i / 64] >> (i % 64)) & 1) mont_mul(c, acc, acc, a);
    }
    l6_copy(r, acc);
}

/* ---- curve ---- */

typedef struct {
    limb6 X, Y, Z;
} pt;

typedef struct {
    mont fp, fn;
    limb6 b;
    pt g;
} curve;

static void curve_init(curve *k) {
    mont_init(&k->fp, P_M);
    mont_init(&k->fn, N_M);
    limb6 t;
    l6_from_be(t, B_BYTES);
    mont_to(&k->fp, k->b, t);
    l6_from_be(t, GX_BYTES);
    mont_to(&k->fp, k->g.X, t);
    l6_from_be(t, GY_BYTES);
    mont_to(&k->fp, k->g.Y, t);
    l6_copy(k->g.Z, k->fp.one);
}

/* Complete addition, RCB 2016 algorithm 4 (a = -3). */
static void pt_add(const curve *k, pt *r, const pt *p, const pt *q) {
    const mont *f = &k->fp;
    limb6 t0, t1, t2, t3, t4, X3, Y3, Z3;
    mont_mul(f, t0, p->X, q->X);
    mont_mul(f, t1, p->Y, q->Y);
    mont_mul(f, t2, p->Z, q->Z);
    mont_add(f, t3, p->X, p->Y);
    mont_add(f, t4, q->X, q->Y);
    mont_mul(f, t3, t3, t4);
    mont_add(f, t4, t0, t1);
    mont_sub(f, t3, t3, t4);
    mont_add(f, t4, p->Y, p->Z);
    mont_add(f, X3, q->Y, q->Z);
    mont_mul(f, t4, t4, X3);
    mont_add(f, X3, t1, t2);
    mont_sub(f, t4, t4, X3);
    mont_add(f, X3, p->X, p->Z);
    mont_add(f, Y3, q->X, q->Z);
    mont_mul(f, X3, X3, Y3);
    mont_add(f, Y3, t0, t2);
    mont_sub(f, Y3, X3, Y3);
    mont_mul(f, Z3, k->b, t2);
    mont_sub(f, X3, Y3, Z3);
    mont_add(f, Z3, X3, X3);
    mont_add(f, X3, X3, Z3);
    mont_sub(f, Z3, t1, X3);
    mont_add(f, X3, t1, X3);
    mont_mul(f, Y3, k->b, Y3);
    mont_add(f, t1, t2, t2);
    mont_add(f, t2, t1, t2);
    mont_sub(f, Y3, Y3, t2);
    mont_sub(f, Y3, Y3, t0);
    mont_add(f, t1, Y3, Y3);
    mont_add(f, Y3, t1, Y3);
    mont_add(f, t1, t0, t0);
    mont_add(f, t0, t1, t0);
    mont_sub(f, t0, t0, t2);
    mont_mul(f, t1, t4, Y3);
    mont_mul(f, t2, t0, Y3);
    mont_mul(f, Y3, X3, Z3);
    mont_add(f, Y3, Y3, t2);
    mont_mul(f, X3, t3, X3);
    mont_sub(f, X3, X3, t1);
    mont_mul(f, Z3, t4, Z3);
    mont_mul(f, t1, t3, t0);
    mont_add(f, Z3, Z3, t1);
    l6_copy(r->X, X3);
    l6_copy(r->Y, Y3);
    l6_copy(r->Z, Z3);
}

static void pt_infinity(const curve *k, pt *r) {
    memset(r, 0, sizeof *r);
    l6_copy(r->Y, k->fp.one);
}

static void pt_cmov(pt *r, const pt *a, uint64_t mask) {
    l6_cmov(r->X, a->X, mask);
    l6_cmov(r->Y, a->Y, mask);
    l6_cmov(r->Z, a->Z, mask);
}

/* r = [s]p, s a 48-byte big-endian scalar; constant time. */
static void pt_mul(const curve *k, pt *r, const uint8_t s[NB], const pt *p) {
    pt tab[16], acc, t;
    pt_infinity(k, &tab[0]);
    tab[1] = *p;
    for (int i = 2; i < 16; i++) pt_add(k, &tab[i], &tab[i - 1], p);
    pt_infinity(k, &acc);
    for (int w = 0; w < 2 * NB; w++) {
        for (int d = 0; d < 4; d++) pt_add(k, &acc, &acc, &acc);
        unsigned nib = (s[w / 2] >> (w % 2 ? 0 : 4)) & 15;
        pt sel;
        memset(&sel, 0, sizeof sel);
        for (unsigned i = 0; i < 16; i++) {
            uint64_t eq = ((uint64_t)(i ^ nib) - 1) >> 63;
            pt_cmov(&sel, &tab[i], 0 - eq);
        }
        pt_add(k, &t, &acc, &sel);
        acc = t;
    }
    *r = acc;
    slang__crypto_wipe(tab, sizeof tab);
}

static bool pt_affine(const curve *k, const pt *p, uint8_t x[NB], uint8_t y[NB]) {
    limb6 zi, t;
    if (l6_is_zero(p->Z)) return false;
    mont_inv(&k->fp, zi, p->Z);
    mont_mul(&k->fp, t, p->X, zi);
    mont_from(&k->fp, t, t);
    l6_to_be(x, t);
    if (y) {
        mont_mul(&k->fp, t, p->Y, zi);
        mont_from(&k->fp, t, t);
        l6_to_be(y, t);
    }
    return true;
}

/* Decodes and validates an uncompressed point. */
static bool pt_decode(const curve *k, pt *r, const uint8_t in[2 * NB + 1]) {
    if (in[0] != 4) return false;
    limb6 x, y;
    l6_from_be(x, in + 1);
    l6_from_be(y, in + 1 + NB);
    if (!l6_lt(x, P_M) || !l6_lt(y, P_M)) return false;
    const mont *f = &k->fp;
    mont_to(f, r->X, x);
    mont_to(f, r->Y, y);
    l6_copy(r->Z, f->one);
    limb6 lhs, rhs, t;
    mont_mul(f, lhs, r->Y, r->Y);
    mont_mul(f, rhs, r->X, r->X);
    mont_mul(f, rhs, rhs, r->X);
    mont_add(f, t, r->X, r->X);
    mont_add(f, t, t, r->X);
    mont_sub(f, rhs, rhs, t);
    mont_add(f, rhs, rhs, k->b);
    limb6 d;
    l6_sub(d, lhs, rhs);
    return l6_is_zero(d) == 1;
}

static bool scalar_ok(const uint8_t s[NB]) {
    limb6 v;
    l6_from_be(v, s);
    return !l6_is_zero(v) && l6_lt(v, N_M);
}

bool slang__p384_pubkey(uint8_t pub[97], const uint8_t priv[48]) {
    if (!scalar_ok(priv)) return false;
    curve k;
    curve_init(&k);
    pt q;
    pt_mul(&k, &q, priv, &k.g);
    pub[0] = 4;
    return pt_affine(&k, &q, pub + 1, pub + 1 + NB);
}

bool slang__p384_ecdh(uint8_t out[48], const uint8_t priv[48], const uint8_t peer[97]) {
    if (!scalar_ok(priv)) return false;
    curve k;
    curve_init(&k);
    pt p, q;
    if (!pt_decode(&k, &p, peer)) return false;
    pt_mul(&k, &q, priv, &p);
    return pt_affine(&k, &q, out, NULL);
}

bool slang__p384_genkey(uint8_t priv[48]) {
    for (int tries = 0; tries < 64; tries++) {
        if (!slang__crypto_random(priv, NB)) return false;
        if (scalar_ok(priv)) return true;
    }
    return false;
}

/* bits2int of the leftmost 384 bits of a digest, reduced modulo n. */
static void digest_to_scalar(limb6 e, const uint8_t *digest, size_t dlen) {
    uint8_t buf[NB] = { 0 };
    if (dlen >= NB) memcpy(buf, digest, NB);
    else memcpy(buf + NB - dlen, digest, dlen);
    l6_from_be(e, buf);
    limb6 d;
    uint64_t borrow = l6_sub(d, e, N_M);
    l6_cmov(e, d, 0 - (borrow ^ 1));
}

static void hmac5(uint8_t out[NB], const uint8_t key[NB], const uint8_t *a, size_t an, const uint8_t *b,
                  size_t bn, const uint8_t *c, size_t cn, const uint8_t *d, size_t dn, const uint8_t *e, size_t en) {
    slang_hmac_ctx h;
    slang__hmac_init(&h, SLANG_HASH_SHA384, key, NB);
    slang__hmac_update(&h, a, an);
    slang__hmac_update(&h, b, bn);
    slang__hmac_update(&h, c, cn);
    slang__hmac_update(&h, d, dn);
    slang__hmac_update(&h, e, en);
    slang__hmac_final(&h, out);
    slang__crypto_wipe(&h, sizeof h);
}

bool slang__p384_sign(uint8_t sig[96], const uint8_t priv[48], const uint8_t *digest, size_t dlen,
                      const uint8_t *extra, size_t elen) {
    if (!scalar_ok(priv)) return false;
    curve k;
    curve_init(&k);
    const mont *fn = &k.fn;
    limb6 e, d;
    digest_to_scalar(e, digest, dlen);
    l6_from_be(d, priv);
    uint8_t h1[NB];
    l6_to_be(h1, e);
    uint8_t V[NB], K[NB], z = 0, one = 1;
    memset(V, 1, NB);
    memset(K, 0, NB);
    if (!extra) elen = 0;
    hmac5(K, K, V, NB, &z, 1, priv, NB, h1, NB, extra, elen);
    hmac5(V, K, V, NB, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    hmac5(K, K, V, NB, &one, 1, priv, NB, h1, NB, extra, elen);
    hmac5(V, K, V, NB, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    bool ok = false;
    for (int tries = 0; tries < 64 && !ok; tries++) {
        hmac5(V, K, V, NB, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
        if (scalar_ok(V)) {
            pt R;
            pt_mul(&k, &R, V, &k.g);
            uint8_t rx[NB];
            if (pt_affine(&k, &R, rx, NULL)) {
                limb6 r, rd, kk, ki, s, t;
                l6_from_be(r, rx);
                uint64_t borrow = l6_sub(rd, r, N_M);
                l6_cmov(r, rd, 0 - (borrow ^ 1));
                if (!l6_is_zero(r)) {
                    l6_from_be(kk, V);
                    mont_to(fn, kk, kk);
                    mont_inv(fn, ki, kk);
                    limb6 rm, dm, em;
                    mont_to(fn, rm, r);
                    mont_to(fn, dm, d);
                    mont_to(fn, em, e);
                    mont_mul(fn, t, rm, dm);
                    mont_add(fn, t, t, em);
                    mont_mul(fn, s, ki, t);
                    mont_from(fn, s, s);
                    if (!l6_is_zero(s)) {
                        l6_to_be(sig, r);
                        l6_to_be(sig + NB, s);
                        ok = true;
                    }
                    slang__crypto_wipe(kk, sizeof kk);
                    slang__crypto_wipe(ki, sizeof ki);
                    slang__crypto_wipe(dm, sizeof dm);
                }
            }
        }
        if (!ok) {
            hmac5(K, K, V, NB, &z, 1, NULL, 0, NULL, 0, NULL, 0);
            hmac5(V, K, V, NB, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
        }
    }
    slang__crypto_wipe(V, sizeof V);
    slang__crypto_wipe(K, sizeof K);
    slang__crypto_wipe(d, sizeof d);
    return ok;
}

bool slang__p384_verify(const uint8_t sig[96], const uint8_t pub[97], const uint8_t *digest, size_t dlen) {
    if (!scalar_ok(sig) || !scalar_ok(sig + NB)) return false;
    curve k;
    curve_init(&k);
    const mont *fn = &k.fn;
    pt Q;
    if (!pt_decode(&k, &Q, pub)) return false;
    limb6 e, r, s, w, u1, u2;
    digest_to_scalar(e, digest, dlen);
    l6_from_be(r, sig);
    l6_from_be(s, sig + NB);
    mont_to(fn, s, s);
    mont_inv(fn, w, s);
    limb6 em, rm;
    mont_to(fn, em, e);
    mont_to(fn, rm, r);
    mont_mul(fn, u1, em, w);
    mont_mul(fn, u2, rm, w);
    mont_from(fn, u1, u1);
    mont_from(fn, u2, u2);
    uint8_t b1[NB], b2[NB];
    l6_to_be(b1, u1);
    l6_to_be(b2, u2);
    pt A, B, X;
    pt_mul(&k, &A, b1, &k.g);
    pt_mul(&k, &B, b2, &Q);
    pt_add(&k, &X, &A, &B);
    uint8_t xb[NB];
    if (!pt_affine(&k, &X, xb, NULL)) return false;
    limb6 x, xd;
    l6_from_be(x, xb);
    uint64_t borrow = l6_sub(xd, x, N_M);
    l6_cmov(x, xd, 0 - (borrow ^ 1));
    limb6 diff;
    l6_sub(diff, x, r);
    return l6_is_zero(diff) == 1;
}
