/* ed25519.c - Ed25519 signatures (RFC 8032 section 5.1), ref10-style:
 * twisted Edwards curve -x^2 + y^2 = 1 + d x^2 y^2 over fe25519.h in
 * extended coordinates (X:Y:Z:T), unified addition (add-2008-hwcd-3),
 * doubling dbl-2008-hwcd. Scalar multiplication is a constant-time
 * double-and-always-add with conditional moves; scalars modulo the group
 * order L are reduced bit-serially with constant-time conditional
 * subtraction. Verification is the cofactorless equation [S]B = R + [k]A
 * (checked as encode([S]B + [k](-A)) == R), with S < L and canonical
 * point encodings required. */
#include "fe25519.h"

#include <string.h>

typedef struct {
    fe X, Y, Z, T;
} ge;

typedef struct {
    fe d, d2, sqrtm1;
    ge base;
} ed_consts;

/* L = 2^252 + 27742317777372353535851937790883648493, little-endian 64-bit limbs. */
static const uint64_t L64[4] = { 0x5812631a5cf5d3edULL, 0x14def9dea2f79cd6ULL, 0, 0x1000000000000000ULL };

/* h = z^e for a public exponent e (32 bytes little-endian). */
static void fe_pow(fe h, const fe z, const uint8_t e[32]) {
    fe r, b;
    fe_1(r);
    fe_copy(b, z);
    for (int i = 0; i < 256; i++) {
        if ((e[i >> 3] >> (i & 7)) & 1) fe_mul(r, r, b);
        fe_sq(b, b);
    }
    fe_copy(h, r);
}

static void ge_identity(ge *p) {
    fe_0(p->X);
    fe_1(p->Y);
    fe_1(p->Z);
    fe_0(p->T);
}

static void ge_add(ge *r, const ge *p, const ge *q, const ed_consts *k) {
    fe a, b, c, d, e, f, g, h, t;
    fe_sub(a, p->Y, p->X);
    fe_sub(t, q->Y, q->X);
    fe_mul(a, a, t);
    fe_add(b, p->Y, p->X);
    fe_add(t, q->Y, q->X);
    fe_mul(b, b, t);
    fe_mul(c, p->T, q->T);
    fe_mul(c, c, k->d2);
    fe_mul(d, p->Z, q->Z);
    fe_add(d, d, d);
    fe_sub(e, b, a);
    fe_sub(f, d, c);
    fe_add(g, d, c);
    fe_add(h, b, a);
    fe_mul(r->X, e, f);
    fe_mul(r->Y, g, h);
    fe_mul(r->T, e, h);
    fe_mul(r->Z, f, g);
}

static void ge_dbl(ge *r, const ge *p) {
    fe a, b, c, d, e, f, g, h, t;
    fe_sq(a, p->X);
    fe_sq(b, p->Y);
    fe_sq(c, p->Z);
    fe_add(c, c, c);
    fe_neg(d, a);
    fe_add(t, p->X, p->Y);
    fe_sq(e, t);
    fe_sub(e, e, a);
    fe_sub(e, e, b);
    fe_add(g, d, b);
    fe_sub(f, g, c);
    fe_sub(h, d, b);
    fe_mul(r->X, e, f);
    fe_mul(r->Y, g, h);
    fe_mul(r->T, e, h);
    fe_mul(r->Z, f, g);
}

static void ge_cmov(ge *r, const ge *p, uint64_t b) {
    fe_cmov(r->X, p->X, b);
    fe_cmov(r->Y, p->Y, b);
    fe_cmov(r->Z, p->Z, b);
    fe_cmov(r->T, p->T, b);
}

/* r = [s]p for a 32-byte little-endian scalar, constant time. */
static void ge_scalarmult(ge *r, const uint8_t s[32], const ge *p, const ed_consts *k) {
    ge acc, t;
    ge_identity(&acc);
    for (int i = 255; i >= 0; i--) {
        ge_dbl(&acc, &acc);
        ge_add(&t, &acc, p, k);
        ge_cmov(&acc, &t, (s[i >> 3] >> (i & 7)) & 1);
    }
    *r = acc;
}

static void ge_encode(uint8_t s[32], const ge *p) {
    fe zi, x, y;
    fe_invert(zi, p->Z);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);
    fe_tobytes(s, y);
    s[31] ^= (uint8_t)(fe_isnegative(x) << 7);
}

/* RFC 8032 5.1.3; false for an invalid or non-canonical encoding. */
static bool ge_decode(ge *p, const uint8_t s[32], const ed_consts *k) {
    uint8_t chk[32];
    int sign = s[31] >> 7;
    fe_frombytes(p->Y, s);
    fe_tobytes(chk, p->Y);
    uint8_t ys[32];
    memcpy(ys, s, 32);
    ys[31] &= 127;
    if (!zell__crypto_ct_eq(chk, ys, 32)) return false;   /* y >= p */
    fe u, v, v3, x, t, one;
    fe_1(one);
    fe_sq(u, p->Y);
    fe_mul(v, u, k->d);
    fe_sub(u, u, one);         /* u = y^2 - 1 */
    fe_add(v, v, one);         /* v = d y^2 + 1 */
    fe_sq(v3, v);
    fe_mul(v3, v3, v);         /* v^3 */
    fe_sq(x, v3);
    fe_mul(x, x, v);
    fe_mul(x, x, u);           /* u v^7 */
    fe_pow22523(x, x);
    fe_mul(x, x, v3);
    fe_mul(x, x, u);           /* u v^3 (u v^7)^((p-5)/8) */
    fe_sq(t, x);
    fe_mul(t, t, v);           /* v x^2 */
    fe chk1, chk2;
    fe_sub(chk1, t, u);
    fe_add(chk2, t, u);
    if (fe_iszero(chk1)) {
        /* x is right */
    } else if (fe_iszero(chk2)) {
        fe_mul(x, x, k->sqrtm1);
    } else {
        return false;
    }
    if (fe_iszero(x) && sign) return false;
    if (fe_isnegative(x) != sign) fe_neg(x, x);
    fe_copy(p->X, x);
    fe_1(p->Z);
    fe_mul(p->T, p->X, p->Y);
    return true;
}

static void consts_init(ed_consts *k) {
    fe num, den, one;
    fe_1(one);
    /* d = -121665 / 121666 */
    fe_0(num);
    num[0] = 121665;
    fe_neg(num, num);
    fe_0(den);
    den[0] = 121666;
    fe_invert(den, den);
    fe_mul(k->d, num, den);
    fe_add(k->d2, k->d, k->d);
    /* sqrt(-1) = 2^((p-1)/4), (p-1)/4 = 2^253 - 5 */
    uint8_t e[32];
    memset(e, 0xff, 32);
    e[0] = 0xfb;
    e[31] = 0x1f;
    fe two;
    fe_0(two);
    two[0] = 2;
    fe_pow(k->sqrtm1, two, e);
    /* base point: y = 4/5, x even */
    uint8_t enc[32];
    fe y, f4, f5;
    fe_0(f4);
    f4[0] = 4;
    fe_0(f5);
    f5[0] = 5;
    fe_invert(f5, f5);
    fe_mul(y, f4, f5);
    fe_tobytes(enc, y);
    (void)ge_decode(&k->base, enc, k);
    (void)one;
}

/* ---- scalars modulo L ---- */

/* out = (in as a little-endian number of n bytes) mod L, constant time. */
static void sc_reduce(uint8_t out[32], const uint8_t *in, size_t n) {
    uint64_t acc[5] = { 0 };
    for (size_t bi = n * 8; bi-- > 0;) {
        /* acc = 2 acc + bit */
        for (int i = 4; i > 0; i--) acc[i] = (acc[i] << 1) | (acc[i - 1] >> 63);
        acc[0] = (acc[0] << 1) | ((in[bi >> 3] >> (bi & 7)) & 1);
        /* acc -= L if acc >= L */
        uint64_t d[5], borrow = 0;
        for (int i = 0; i < 5; i++) {
            uint64_t li = i < 4 ? L64[i] : 0;
            u128 t = (u128)acc[i] - li - borrow;
            d[i] = (uint64_t)t;
            borrow = (uint64_t)(t >> 64) & 1;
        }
        uint64_t keep = 0 - borrow;   /* borrow: acc < L, keep acc */
        for (int i = 0; i < 5; i++) acc[i] = (acc[i] & keep) | (d[i] & ~keep);
    }
    for (int i = 0; i < 4; i++) zell__st64le(out + 8 * i, acc[i]);
    zell__crypto_wipe(acc, sizeof acc);
}

/* s = (a * b + c) mod L. */
static void sc_muladd(uint8_t s[32], const uint8_t a[32], const uint8_t b[32], const uint8_t c[32]) {
    uint64_t x[4], y[4], z[9] = { 0 };
    for (int i = 0; i < 4; i++) {
        x[i] = zell__ld64le(a + 8 * i);
        y[i] = zell__ld64le(b + 8 * i);
    }
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            u128 t = (u128)x[i] * y[j] + z[i + j] + carry;
            z[i + j] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        z[i + 4] = carry;
    }
    uint64_t carry = 0;
    for (int i = 0; i < 9; i++) {
        u128 t = (u128)z[i] + (i < 4 ? zell__ld64le(c + 8 * i) : 0) + carry;
        z[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
    uint8_t buf[72];
    for (int i = 0; i < 9; i++) zell__st64le(buf + 8 * i, z[i]);
    sc_reduce(s, buf, 72);
    zell__crypto_wipe(buf, sizeof buf);
    zell__crypto_wipe(z, sizeof z);
    zell__crypto_wipe(x, sizeof x);
}

static bool sc_is_canonical(const uint8_t s[32]) {
    for (int i = 3; i >= 0; i--) {
        uint64_t v = zell__ld64le(s + 8 * i);
        if (v < L64[i]) return true;
        if (v > L64[i]) return false;
    }
    return false;   /* equal to L */
}

/* ---- API ---- */

static void expand_seed(const uint8_t seed[32], uint8_t a[32], uint8_t prefix[32]) {
    uint8_t h[64];
    zell__hash(ZELL_HASH_SHA512, seed, 32, h);
    memcpy(a, h, 32);
    a[0] &= 248;
    a[31] &= 127;
    a[31] |= 64;
    memcpy(prefix, h + 32, 32);
    zell__crypto_wipe(h, sizeof h);
}

void zell__ed25519_pubkey(uint8_t pub[32], const uint8_t seed[32]) {
    ed_consts k;
    consts_init(&k);
    uint8_t a[32], prefix[32];
    expand_seed(seed, a, prefix);
    ge A;
    ge_scalarmult(&A, a, &k.base, &k);
    ge_encode(pub, &A);
    zell__crypto_wipe(a, sizeof a);
    zell__crypto_wipe(prefix, sizeof prefix);
}

void zell__ed25519_sign(uint8_t sig[64], const uint8_t seed[32], const uint8_t *msg, size_t n) {
    ed_consts k;
    consts_init(&k);
    uint8_t a[32], prefix[32], pub[32], h[64], r[32], kk[32];
    expand_seed(seed, a, prefix);
    ge P;
    ge_scalarmult(&P, a, &k.base, &k);
    ge_encode(pub, &P);
    zell_hash_ctx c;
    zell__hash_init(&c, ZELL_HASH_SHA512);
    zell__hash_update(&c, prefix, 32);
    zell__hash_update(&c, msg, n);
    zell__hash_final(&c, h);
    sc_reduce(r, h, 64);
    ge_scalarmult(&P, r, &k.base, &k);
    ge_encode(sig, &P);
    zell__hash_init(&c, ZELL_HASH_SHA512);
    zell__hash_update(&c, sig, 32);
    zell__hash_update(&c, pub, 32);
    zell__hash_update(&c, msg, n);
    zell__hash_final(&c, h);
    sc_reduce(kk, h, 64);
    sc_muladd(sig + 32, kk, a, r);
    zell__crypto_wipe(a, sizeof a);
    zell__crypto_wipe(prefix, sizeof prefix);
    zell__crypto_wipe(r, sizeof r);
    zell__crypto_wipe(h, sizeof h);
    zell__crypto_wipe(&c, sizeof c);
}

bool zell__ed25519_verify(const uint8_t sig[64], const uint8_t pub[32], const uint8_t *msg, size_t n) {
    ed_consts k;
    consts_init(&k);
    ge A, R;
    if (!sc_is_canonical(sig + 32)) return false;
    if (!ge_decode(&A, pub, &k) || !ge_decode(&R, sig, &k)) return false;
    uint8_t h[64], kk[32], chk[32];
    zell_hash_ctx c;
    zell__hash_init(&c, ZELL_HASH_SHA512);
    zell__hash_update(&c, sig, 32);
    zell__hash_update(&c, pub, 32);
    zell__hash_update(&c, msg, n);
    zell__hash_final(&c, h);
    sc_reduce(kk, h, 64);
    /* [S]B + [k](-A) */
    fe_neg(A.X, A.X);
    fe_neg(A.T, A.T);
    ge sb, ka, sum;
    ge_scalarmult(&sb, sig + 32, &k.base, &k);
    ge_scalarmult(&ka, kk, &A, &k);
    ge_add(&sum, &sb, &ka, &k);
    ge_encode(chk, &sum);
    return zell__crypto_ct_eq(chk, sig, 32) == 1;
}
