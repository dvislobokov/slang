/* x25519.c - the X25519 function (RFC 7748 section 5): a constant-time
 * Montgomery ladder over fe25519.h. */
#include "fe25519.h"

#include <string.h>

bool zell__x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;
    fe x1, x2, z2, x3, z3, a, aa, b, bb, ee, c, d, da, cb, t;
    fe_frombytes(x1, point);
    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);
    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;
        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(ee, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(t, da, cb);
        fe_sq(x3, t);
        fe_sub(t, da, cb);
        fe_sq(t, t);
        fe_mul(z3, x1, t);
        fe_mul(x2, aa, bb);
        fe_mul_small(t, ee, 121665);
        fe_add(t, aa, t);
        fe_mul(z2, ee, t);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
    zell__crypto_wipe(e, sizeof e);
    uint8_t zero[32] = { 0 };
    return !zell__crypto_ct_eq(out, zero, 32);
}

void zell__x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t base[32] = { 9 };
    (void)zell__x25519(out, scalar, base);
}
