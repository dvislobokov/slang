/* hkdf.c - HKDF-Extract and HKDF-Expand (RFC 5869) over HMAC-SHA-2. */
#include "crypto.h"

#include <string.h>

void slang__hkdf_extract(int alg, const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen,
                         uint8_t *prk) {
    uint8_t zeros[64] = { 0 };
    if (slen == 0) {   /* RFC 5869 2.2: a string of HashLen zeros */
        salt = zeros;
        slen = slang__hash_size(alg);
    }
    slang__hmac(alg, salt, slen, ikm, ilen, prk);
}

bool slang__hkdf_expand(int alg, const uint8_t *prk, size_t plen, const uint8_t *info, size_t ilen,
                        uint8_t *out, size_t n) {
    size_t hl = slang__hash_size(alg);
    if (hl == 0 || n > 255 * hl) return false;
    uint8_t t[64];
    size_t tlen = 0, done = 0;
    for (uint8_t i = 1; done < n; i++) {
        slang_hmac_ctx c;
        slang__hmac_init(&c, alg, prk, plen);
        slang__hmac_update(&c, t, tlen);
        slang__hmac_update(&c, info, ilen);
        slang__hmac_update(&c, &i, 1);
        slang__hmac_final(&c, t);
        slang__crypto_wipe(&c, sizeof c);
        tlen = hl;
        size_t k = n - done < hl ? n - done : hl;
        memcpy(out + done, t, k);
        done += k;
    }
    slang__crypto_wipe(t, sizeof t);
    return true;
}
