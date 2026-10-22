/* std_hash.c - SipHash-1-3 for hash.maphash (one compression round,
 * three finalization rounds; the variant Rust's HashMap uses). Input is
 * read little-endian, as in the reference implementation. */
#include "rt_internal.h"

#include <string.h>

uint64_t zell_hash_maphash_rt_sip13(uint64_t k0, uint64_t k1, const zell_slice *b);
uint64_t zell_hash_maphash_rt_sip13_str(uint64_t k0, uint64_t k1, const zell_string *s);

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND                                                     \
    do {                                                             \
        v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32);    \
        v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2;                       \
        v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0;                       \
        v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32);    \
    } while (0)

static uint64_t sip13(uint64_t k0, uint64_t k1, const uint8_t *p, int64_t n) {
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    int64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t m = 0;
        for (int j = 0; j < 8; j++) m |= (uint64_t)p[i + j] << (8 * j);
        v3 ^= m;
        SIPROUND;
        v0 ^= m;
    }
    uint64_t last = (uint64_t)(n & 0xff) << 56;
    for (int j = 0; i + j < n; j++) last |= (uint64_t)p[i + j] << (8 * j);
    v3 ^= last;
    SIPROUND;
    v0 ^= last;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

uint64_t zell_hash_maphash_rt_sip13(uint64_t k0, uint64_t k1, const zell_slice *b) {
    return sip13(k0, k1, b && b->len ? b->data : NULL, b ? b->len : 0);
}

uint64_t zell_hash_maphash_rt_sip13_str(uint64_t k0, uint64_t k1, const zell_string *s) {
    return sip13(k0, k1, (const uint8_t *)(s ? s->data : ""), s ? s->len : 0);
}
