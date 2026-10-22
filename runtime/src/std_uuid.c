/* std_uuid.c - std.uuid (zell_uuid_*): RFC 4122 / 9562 version-4 UUIDs from
 * std.crypto.rand, canonical lowercase 8-4-4-4-12 form. */
#include "rt_internal.h"

#include <string.h>

zell_string *zell_uuid_new(void) {
    zell_slice b;
    zell_error *e = NULL;
    zell_crypto_rand_bytes(16, &b, &e);
    if (e) zell_panic_error(e);   /* no entropy source: unrecoverable */
    uint8_t *u = b.data;
    u[6] = (uint8_t)((u[6] & 0x0F) | 0x40);   /* version 4 */
    u[8] = (uint8_t)((u[8] & 0x3F) | 0x80);   /* variant 10xx */
    static const char hx[] = "0123456789abcdef";
    zell_string *r = zell__string_alloc(36);
    char *w = r->data;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) *w++ = '-';
        *w++ = hx[u[i] >> 4];
        *w++ = hx[u[i] & 15];
    }
    zell_slice_release(&b);
    return r;
}

/* Canonical 36-char hex form, any case, any version/variant. */
bool zell_uuid_is_valid(const zell_string *s) {
    if (zell_string_len(s) != 36) return false;
    const char *p = zell_string_data(s);
    for (int i = 0; i < 36; i++) {
        char c = p[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}
