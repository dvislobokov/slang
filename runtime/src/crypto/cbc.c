/* cbc.c - AES decryption (FIPS 197 5.3), AES-CBC (SP 800-38A 6.2) and
 * PKCS#7 padding (RFC 5652 6.3), for legacy formats (crypto.aes
 * CbcEncrypt/CbcDecrypt).
 *
 * Decryption uses AES-NI (aesdec with InvMixColumns'd round keys) when the
 * key was set up for it, otherwise a byte-oriented inverse cipher whose
 * InvSubBytes computes the GF(2^8) inverse by a fixed exponentiation
 * (x^254) with branch-free multiplications: no table lookups by secret
 * data. PKCS#7 unpadding checks the padding in constant time; CBC without
 * a MAC is still malleable and a decryption error is a padding oracle, so
 * new formats should use GCM. */
#include "crypto.h"

#include <string.h>

static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        r ^= a & (uint8_t)(0 - (b & 1));
        uint8_t hi = (uint8_t)(0 - (a >> 7));
        a = (uint8_t)((a << 1) ^ (0x1b & hi));
        b >>= 1;
    }
    return r;
}

/* a^254 = a^-1 in GF(2^8) (0 -> 0). */
static uint8_t gf_inv(uint8_t a) {
    uint8_t a2 = gf_mul(a, a), a4 = gf_mul(a2, a2), a8 = gf_mul(a4, a4);
    uint8_t a16 = gf_mul(a8, a8), a32 = gf_mul(a16, a16), a64 = gf_mul(a32, a32), a128 = gf_mul(a64, a64);
    uint8_t r = gf_mul(a128, a64);
    r = gf_mul(r, a32);
    r = gf_mul(r, a16);
    r = gf_mul(r, a8);
    r = gf_mul(r, a4);
    return gf_mul(r, a2);
}

static inline uint8_t rotl8(uint8_t x, int k) { return (uint8_t)((x << k) | (x >> (8 - k))); }

static uint8_t inv_sbox(uint8_t y) {
    uint8_t x = (uint8_t)(rotl8(y, 1) ^ rotl8(y, 3) ^ rotl8(y, 6) ^ 0x05);
    return gf_inv(x);
}

static void inv_mix(uint8_t *s) {
    for (int c = 0; c < 4; c++) {
        uint8_t *col = s + 4 * c;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9);
        col[1] = gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13);
        col[2] = gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11);
        col[3] = gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14);
    }
}

/* State byte i is row i%4, column i/4; InvShiftRows moves row r right by r. */
static void inv_shift_sub(uint8_t *s) {
    uint8_t t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) t[4 * ((c + r) & 3) + r] = s[4 * c + r];
    for (int i = 0; i < 16; i++) s[i] = inv_sbox(t[i]);
}

static void pt_decrypt(const zell_aes_key *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    unsigned nr = k->nr;
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ k->rk[16 * nr + i];
    for (unsigned r = nr - 1; r >= 1; r--) {
        inv_shift_sub(s);
        for (int i = 0; i < 16; i++) s[i] ^= k->rk[16 * r + i];
        inv_mix(s);
    }
    inv_shift_sub(s);
    for (int i = 0; i < 16; i++) out[i] = s[i] ^ k->rk[i];
    zell__crypto_wipe(s, sizeof s);
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define ZELL_HAVE_AESNI_DEC 1
#include <wmmintrin.h>
#define NI_DEC __attribute__((target("aes,sse2")))

NI_DEC static void ni_decrypt(const zell_aes_key *k, const uint8_t in[16], uint8_t out[16]) {
    unsigned nr = k->nr;
    __m128i b = _mm_loadu_si128((const __m128i *)in);
    b = _mm_xor_si128(b, _mm_loadu_si128((const __m128i *)(k->rk + 16 * nr)));
    for (unsigned r = nr - 1; r >= 1; r--)
        b = _mm_aesdec_si128(b, _mm_aesimc_si128(_mm_loadu_si128((const __m128i *)(k->rk + 16 * r))));
    b = _mm_aesdeclast_si128(b, _mm_loadu_si128((const __m128i *)k->rk));
    _mm_storeu_si128((__m128i *)out, b);
}
#endif

void zell__aes_decrypt_block(const zell_aes_key *k, const uint8_t in[16], uint8_t out[16]) {
#ifdef ZELL_HAVE_AESNI_DEC
    if (k->ni) {
        ni_decrypt(k, in, out);
        return;
    }
#endif
    pt_decrypt(k, in, out);
}

void zell__cbc_encrypt(const zell_aes_key *k, const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t x[16];
    memcpy(x, iv, 16);
    for (size_t off = 0; off + 16 <= n; off += 16) {
        for (int i = 0; i < 16; i++) x[i] ^= in[off + i];
        zell__aes_encrypt_block(k, x, x);
        memcpy(out + off, x, 16);
    }
}

void zell__cbc_decrypt(const zell_aes_key *k, const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out) {
    uint8_t prev[16], cur[16], x[16];
    memcpy(prev, iv, 16);
    for (size_t off = 0; off + 16 <= n; off += 16) {
        memcpy(cur, in + off, 16);
        zell__aes_decrypt_block(k, cur, x);
        for (int i = 0; i < 16; i++) out[off + i] = x[i] ^ prev[i];
        memcpy(prev, cur, 16);
    }
    zell__crypto_wipe(x, sizeof x);
}

/* Length of the PKCS#7 padding of a block-aligned buffer, or 0 if the
 * padding is invalid; constant time in the padding bytes. */
size_t zell__pkcs7_unpad_len(const uint8_t *p, size_t n, size_t block) {
    if (n == 0 || n % block != 0 || block == 0 || block > 255) return 0;
    uint8_t pad = p[n - 1];
    uint32_t bad = (uint32_t)((pad - 1u) >> 8 & 1) | (uint32_t)(((uint32_t)block - pad) >> 31);   /* pad == 0 || pad > block */
    for (size_t i = 0; i < block; i++) {
        uint8_t b = p[n - 1 - i];
        /* inside: i < pad */
        uint32_t inside = (uint32_t)((i - (size_t)pad) >> (sizeof(size_t) * 8 - 1)) & 1;
        bad |= inside & (uint32_t)(((b ^ pad) + 0xffu) >> 8);
    }
    return bad ? 0 : pad;
}
