/* hkdf.c - HKDF-Extract and HKDF-Expand (RFC 5869) over HMAC-SHA-2. */
#include "crypto.h"

#include <string.h>

void zell__hkdf_extract(int alg, const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen,
                         uint8_t *prk) {
    uint8_t zeros[64] = { 0 };
    if (slen == 0) {   /* RFC 5869 2.2: a string of HashLen zeros */
        salt = zeros;
        slen = zell__hash_size(alg);
    }
    zell__hmac(alg, salt, slen, ikm, ilen, prk);
}

bool zell__hkdf_expand(int alg, const uint8_t *prk, size_t plen, const uint8_t *info, size_t ilen,
                        uint8_t *out, size_t n) {
    size_t hl = zell__hash_size(alg);
    if (hl == 0 || n > 255 * hl) return false;
    uint8_t t[64];
    size_t tlen = 0, done = 0;
    for (uint8_t i = 1; done < n; i++) {
        zell_hmac_ctx c;
        zell__hmac_init(&c, alg, prk, plen);
        zell__hmac_update(&c, t, tlen);
        zell__hmac_update(&c, info, ilen);
        zell__hmac_update(&c, &i, 1);
        zell__hmac_final(&c, t);
        zell__crypto_wipe(&c, sizeof c);
        tlen = hl;
        size_t k = n - done < hl ? n - done : hl;
        memcpy(out + done, t, k);
        done += k;
    }
    zell__crypto_wipe(t, sizeof t);
    return true;
}
