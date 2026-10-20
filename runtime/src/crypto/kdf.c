/* kdf.c - password-based key derivation: PBKDF2 (RFC 8018 5.2) over
 * HMAC-SHA1/256/384/512 and scrypt (RFC 7914) over PBKDF2-HMAC-SHA256
 * and Salsa20/8. Argon2 is in std_argon2.c. */
#include "crypto.h"

#include <stdlib.h>
#include <string.h>

bool slang__pbkdf2(int alg, const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen, uint64_t iter,
                   uint8_t *out, size_t n) {
    size_t hl = slang__hash_size(alg);
    if (!hl || iter == 0) return false;
    slang_hmac_ctx base, c;
    slang__hmac_init(&base, alg, pw, pwlen);
    uint8_t u[64], t[64], ctr[4];
    for (uint32_t block = 1; n > 0; block++) {
        slang__st32be(ctr, block);
        c = base;
        slang__hmac_update(&c, salt, slen);
        slang__hmac_update(&c, ctr, 4);
        slang__hmac_final(&c, u);
        memcpy(t, u, hl);
        for (uint64_t i = 1; i < iter; i++) {
            c = base;
            slang__hmac_update(&c, u, hl);
            slang__hmac_final(&c, u);
            for (size_t j = 0; j < hl; j++) t[j] ^= u[j];
        }
        size_t k = n < hl ? n : hl;
        memcpy(out, t, k);
        out += k;
        n -= k;
    }
    slang__crypto_wipe(&base, sizeof base);
    slang__crypto_wipe(&c, sizeof c);
    slang__crypto_wipe(u, sizeof u);
    slang__crypto_wipe(t, sizeof t);
    return true;
}

/* ---- scrypt ---- */

#define R32(a, b) (((a) << (b)) | ((a) >> (32 - (b))))

static void salsa20_8(uint32_t B[16]) {
    uint32_t x[16];
    memcpy(x, B, sizeof x);
    for (int i = 0; i < 8; i += 2) {
        x[4] ^= R32(x[0] + x[12], 7);  x[8] ^= R32(x[4] + x[0], 9);
        x[12] ^= R32(x[8] + x[4], 13); x[0] ^= R32(x[12] + x[8], 18);
        x[9] ^= R32(x[5] + x[1], 7);   x[13] ^= R32(x[9] + x[5], 9);
        x[1] ^= R32(x[13] + x[9], 13); x[5] ^= R32(x[1] + x[13], 18);
        x[14] ^= R32(x[10] + x[6], 7); x[2] ^= R32(x[14] + x[10], 9);
        x[6] ^= R32(x[2] + x[14], 13); x[10] ^= R32(x[6] + x[2], 18);
        x[3] ^= R32(x[15] + x[11], 7); x[7] ^= R32(x[3] + x[15], 9);
        x[11] ^= R32(x[7] + x[3], 13); x[15] ^= R32(x[11] + x[7], 18);
        x[1] ^= R32(x[0] + x[3], 7);   x[2] ^= R32(x[1] + x[0], 9);
        x[3] ^= R32(x[2] + x[1], 13);  x[0] ^= R32(x[3] + x[2], 18);
        x[6] ^= R32(x[5] + x[4], 7);   x[7] ^= R32(x[6] + x[5], 9);
        x[4] ^= R32(x[7] + x[6], 13);  x[5] ^= R32(x[4] + x[7], 18);
        x[11] ^= R32(x[10] + x[9], 7); x[8] ^= R32(x[11] + x[10], 9);
        x[9] ^= R32(x[8] + x[11], 13); x[10] ^= R32(x[9] + x[8], 18);
        x[12] ^= R32(x[15] + x[14], 7); x[13] ^= R32(x[12] + x[15], 9);
        x[14] ^= R32(x[13] + x[12], 13); x[15] ^= R32(x[14] + x[13], 18);
    }
    for (int i = 0; i < 16; i++) B[i] += x[i];
}

/* BlockMix of 2r 64-byte blocks in B (words) into Y. */
static void block_mix(const uint32_t *B, uint32_t *Y, size_t r) {
    uint32_t X[16];
    memcpy(X, B + (2 * r - 1) * 16, 64);
    for (size_t i = 0; i < 2 * r; i++) {
        for (int j = 0; j < 16; j++) X[j] ^= B[i * 16 + j];
        salsa20_8(X);
        /* even blocks first, then odd */
        memcpy(Y + ((i & 1) * r + i / 2) * 16, X, 64);
    }
}

static void romix(uint8_t *Bbytes, size_t r, uint64_t N, uint32_t *V, uint32_t *XY) {
    size_t words = 32 * r;
    uint32_t *X = XY, *Y = XY + words;
    for (size_t k = 0; k < words; k++) X[k] = slang__ld32le(Bbytes + 4 * k);
    for (uint64_t i = 0; i < N; i++) {
        memcpy(V + i * words, X, words * 4);
        block_mix(X, Y, r);
        memcpy(X, Y, words * 4);
    }
    for (uint64_t i = 0; i < N; i++) {
        uint64_t j = X[(2 * r - 1) * 16] & (N - 1);
        for (size_t k = 0; k < words; k++) X[k] ^= V[j * words + k];
        block_mix(X, Y, r);
        memcpy(X, Y, words * 4);
    }
    for (size_t k = 0; k < words; k++) slang__st32le(Bbytes + 4 * k, X[k]);
}

/* 0 ok, 1 bad parameters, 2 out of memory. */
int slang__scrypt(const uint8_t *pw, size_t pwlen, const uint8_t *salt, size_t slen, uint64_t N, uint64_t r,
                  uint64_t p, uint8_t *out, size_t n) {
    if (N < 2 || (N & (N - 1)) || r == 0 || p == 0 || r * p >= (1u << 30) || n == 0) return 1;
    if (r > ((size_t)1 << 24) || N > ((uint64_t)1 << 40) / (128 * r)) return 1;
    size_t blen = 128 * r * p;
    uint8_t *B = malloc(blen);
    uint32_t *V = malloc((size_t)(128 * r * N));
    uint32_t *XY = malloc(256 * r);
    if (!B || !V || !XY) {
        free(B); free(V); free(XY);
        return 2;
    }
    slang__pbkdf2(SLANG_HASH_SHA256, pw, pwlen, salt, slen, 1, B, blen);
    for (uint64_t i = 0; i < p; i++) romix(B + i * 128 * r, (size_t)r, N, V, XY);
    slang__pbkdf2(SLANG_HASH_SHA256, pw, pwlen, B, blen, 1, out, n);
    slang__crypto_wipe(B, blen);
    slang__crypto_wipe(V, (size_t)(128 * r * N));
    slang__crypto_wipe(XY, 256 * r);
    free(B);
    free(V);
    free(XY);
    return 0;
}
