/* fe25519.h - arithmetic modulo p = 2^255 - 19 for X25519 and Ed25519.
 *
 * Five 51-bit limbs (radix 2^51), products in unsigned __int128, as in
 * curve25519-donna-c64 / ref10's 64-bit variant. Every function is constant
 * time; elements are kept loosely reduced (limbs < 2^52) between operations
 * and fully reduced only by fe_tobytes. Included by x25519.c and ed25519.c. */
#ifndef ZELL_FE25519_H
#define ZELL_FE25519_H

#include "crypto.h"

typedef uint64_t fe[5];
typedef unsigned __int128 u128;

#define FE_MASK51 ((uint64_t)0x7ffffffffffff)

static inline void fe_0(fe h) { h[0] = h[1] = h[2] = h[3] = h[4] = 0; }
static inline void fe_1(fe h) { h[0] = 1; h[1] = h[2] = h[3] = h[4] = 0; }
static inline void fe_copy(fe h, const fe f) { for (int i = 0; i < 5; i++) h[i] = f[i]; }

static inline void fe_frombytes(fe h, const uint8_t s[32]) {
    uint64_t a = zell__ld64le(s), b = zell__ld64le(s + 8), c = zell__ld64le(s + 16), d = zell__ld64le(s + 24);
    h[0] = a & FE_MASK51;
    h[1] = ((a >> 51) | (b << 13)) & FE_MASK51;
    h[2] = ((b >> 38) | (c << 26)) & FE_MASK51;
    h[3] = ((c >> 25) | (d << 39)) & FE_MASK51;
    h[4] = (d >> 12) & FE_MASK51;   /* the top bit is ignored */
}

static inline void fe_carry(fe h) {
    uint64_t c;
    c = h[0] >> 51; h[0] &= FE_MASK51; h[1] += c;
    c = h[1] >> 51; h[1] &= FE_MASK51; h[2] += c;
    c = h[2] >> 51; h[2] &= FE_MASK51; h[3] += c;
    c = h[3] >> 51; h[3] &= FE_MASK51; h[4] += c;
    c = h[4] >> 51; h[4] &= FE_MASK51; h[0] += c * 19;
}

static inline void fe_tobytes(uint8_t s[32], const fe f) {
    fe t;
    fe_copy(t, f);
    fe_carry(t);
    fe_carry(t);
    /* now t < 2^255 + small; subtract p if t >= p (constant time) */
    uint64_t q = (t[0] + 19) >> 51;
    q = (t[1] + q) >> 51;
    q = (t[2] + q) >> 51;
    q = (t[3] + q) >> 51;
    q = (t[4] + q) >> 51;
    t[0] += 19 * q;
    uint64_t c;
    c = t[0] >> 51; t[0] &= FE_MASK51; t[1] += c;
    c = t[1] >> 51; t[1] &= FE_MASK51; t[2] += c;
    c = t[2] >> 51; t[2] &= FE_MASK51; t[3] += c;
    c = t[3] >> 51; t[3] &= FE_MASK51; t[4] += c;
    t[4] &= FE_MASK51;
    zell__st64le(s, t[0] | (t[1] << 51));
    zell__st64le(s + 8, (t[1] >> 13) | (t[2] << 38));
    zell__st64le(s + 16, (t[2] >> 26) | (t[3] << 25));
    zell__st64le(s + 24, (t[3] >> 39) | (t[4] << 12));
}

/* h = f + g, carried so every limb stays below 2^52 (fe_sub relies on it). */
static inline void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 5; i++) h[i] = f[i] + g[i];
    fe_carry(h);
}

/* h = f - g, adding 4p first so limbs stay positive (inputs < 2^53). */
static inline void fe_sub(fe h, const fe f, const fe g) {
    h[0] = f[0] + 0x1fffffffffffb4ULL - g[0];
    h[1] = f[1] + 0x1ffffffffffffcULL - g[1];
    h[2] = f[2] + 0x1ffffffffffffcULL - g[2];
    h[3] = f[3] + 0x1ffffffffffffcULL - g[3];
    h[4] = f[4] + 0x1ffffffffffffcULL - g[4];
    fe_carry(h);
}

static inline void fe_neg(fe h, const fe f) {
    fe z;
    fe_0(z);
    fe_sub(h, z, f);
}

static inline void fe_mul(fe h, const fe f, const fe g) {
    uint64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    uint64_t g1_19 = 19 * g1, g2_19 = 19 * g2, g3_19 = 19 * g3, g4_19 = 19 * g4;
    u128 r0 = (u128)f0 * g0 + (u128)f1 * g4_19 + (u128)f2 * g3_19 + (u128)f3 * g2_19 + (u128)f4 * g1_19;
    u128 r1 = (u128)f0 * g1 + (u128)f1 * g0 + (u128)f2 * g4_19 + (u128)f3 * g3_19 + (u128)f4 * g2_19;
    u128 r2 = (u128)f0 * g2 + (u128)f1 * g1 + (u128)f2 * g0 + (u128)f3 * g4_19 + (u128)f4 * g3_19;
    u128 r3 = (u128)f0 * g3 + (u128)f1 * g2 + (u128)f2 * g1 + (u128)f3 * g0 + (u128)f4 * g4_19;
    u128 r4 = (u128)f0 * g4 + (u128)f1 * g3 + (u128)f2 * g2 + (u128)f3 * g1 + (u128)f4 * g0;
    uint64_t c;
    c = (uint64_t)(r0 >> 51); h[0] = (uint64_t)r0 & FE_MASK51; r1 += c;
    c = (uint64_t)(r1 >> 51); h[1] = (uint64_t)r1 & FE_MASK51; r2 += c;
    c = (uint64_t)(r2 >> 51); h[2] = (uint64_t)r2 & FE_MASK51; r3 += c;
    c = (uint64_t)(r3 >> 51); h[3] = (uint64_t)r3 & FE_MASK51; r4 += c;
    c = (uint64_t)(r4 >> 51); h[4] = (uint64_t)r4 & FE_MASK51;
    h[0] += c * 19;
    c = h[0] >> 51; h[0] &= FE_MASK51; h[1] += c;
}

static inline void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

static inline void fe_mul_small(fe h, const fe f, uint64_t k) {
    u128 r[5];
    for (int i = 0; i < 5; i++) r[i] = (u128)f[i] * k;
    uint64_t c = 0;
    for (int i = 0; i < 5; i++) {
        r[i] += c;
        h[i] = (uint64_t)r[i] & FE_MASK51;
        c = (uint64_t)(r[i] >> 51);
    }
    h[0] += c * 19;
    c = h[0] >> 51; h[0] &= FE_MASK51; h[1] += c;
}

/* Conditional swap/move with b in {0, 1}. */
static inline void fe_cswap(fe f, fe g, uint64_t b) {
    uint64_t m = 0 - b;
    for (int i = 0; i < 5; i++) {
        uint64_t x = (f[i] ^ g[i]) & m;
        f[i] ^= x;
        g[i] ^= x;
    }
}

static inline void fe_cmov(fe f, const fe g, uint64_t b) {
    uint64_t m = 0 - b;
    for (int i = 0; i < 5; i++) f[i] ^= (f[i] ^ g[i]) & m;
}

static inline void fe_sqn(fe h, const fe f, int n) {
    fe_sq(h, f);
    for (int i = 1; i < n; i++) fe_sq(h, h);
}

/* z^(2^250 - 1) helper chain shared by invert and pow22523. */
static inline void fe_pow250(fe t2_250, fe z11, const fe z) {
    fe t0, t1, z9, t;
    fe_sq(t0, z);              /* 2 */
    fe_sqn(t1, t0, 2);         /* 8 */
    fe_mul(z9, t1, z);         /* 9 */
    fe_mul(z11, z9, t0);       /* 11 */
    fe_sq(t0, z11);            /* 22 */
    fe_mul(t, t0, z9);         /* 2^5 - 1 */
    fe_sqn(t0, t, 5);
    fe_mul(t, t0, t);          /* 2^10 - 1 */
    fe_sqn(t0, t, 10);
    fe_mul(t1, t0, t);         /* 2^20 - 1 */
    fe_sqn(t0, t1, 20);
    fe_mul(t0, t0, t1);        /* 2^40 - 1 */
    fe_sqn(t0, t0, 10);
    fe_mul(t, t0, t);          /* 2^50 - 1 */
    fe_sqn(t0, t, 50);
    fe_mul(t1, t0, t);         /* 2^100 - 1 */
    fe_sqn(t0, t1, 100);
    fe_mul(t0, t0, t1);        /* 2^200 - 1 */
    fe_sqn(t0, t0, 50);
    fe_mul(t2_250, t0, t);     /* 2^250 - 1 */
}

/* h = z^(p-2) = z^(2^255 - 21). */
static inline void fe_invert(fe h, const fe z) {
    fe t, z11;
    fe_pow250(t, z11, z);
    fe_sqn(t, t, 5);           /* 2^255 - 2^5 */
    fe_mul(h, t, z11);         /* 2^255 - 21 */
}

/* h = z^((p-5)/8) = z^(2^252 - 3). */
static inline void fe_pow22523(fe h, const fe z) {
    fe t, z11;
    fe_pow250(t, z11, z);
    fe_sqn(t, t, 2);           /* 2^252 - 4 */
    fe_mul(h, t, z);           /* 2^252 - 3 */
}

static inline int fe_isnegative(const fe f) {
    uint8_t s[32];
    fe_tobytes(s, f);
    return s[0] & 1;
}

static inline int fe_iszero(const fe f) {
    uint8_t s[32], z[32] = { 0 };
    fe_tobytes(s, f);
    return zell__crypto_ct_eq(s, z, 32);
}

#endif
