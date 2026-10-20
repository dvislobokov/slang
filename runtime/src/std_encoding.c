/* std_encoding.c - std.encoding.base64 and std.encoding.hex
 * (slang_encoding_base64_*, slang_encoding_hex_*), RFC 4648.
 * Standard base64 is padded (Go base64.StdEncoding); the URL variant uses the
 * URL-safe alphabet without padding (Go base64.RawURLEncoding), and DecodeURL
 * also accepts padded input. Decoders skip '\r' and '\n'. */
#include "rt_internal.h"

#include <stdio.h>
#include <string.h>

static inline int64_t slen(const slang_string *s) { return s ? s->len : 0; }
static inline const uint8_t *sbytes(const slang_string *s) {
    return (const uint8_t *)(s ? s->data : "");
}
static inline int64_t blen(const slang_slice *b) { return b ? b->len : 0; }
static inline const uint8_t *bbytes(const slang_slice *b) {
    return b && b->len ? (const uint8_t *)b->data : (const uint8_t *)"";
}

static const char STD_ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char URL_ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static slang_string *b64_encode(const uint8_t *p, int64_t n, const char *alpha, bool padded) {
    if (n > (INT64_MAX / 4) * 3 - 3) slang_panic("base64: input too large");
    int64_t full = n / 3, rem = n % 3;
    int64_t outlen = full * 4 + (rem ? (padded ? 4 : rem + 1) : 0);
    slang_string *r = slang__string_alloc(outlen);
    char *w = r->data;
    int64_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        *w++ = alpha[v >> 18]; *w++ = alpha[(v >> 12) & 63];
        *w++ = alpha[(v >> 6) & 63]; *w++ = alpha[v & 63];
    }
    if (rem) {
        uint32_t v = (uint32_t)p[i] << 16 | (rem == 2 ? (uint32_t)p[i + 1] << 8 : 0);
        *w++ = alpha[v >> 18];
        *w++ = alpha[(v >> 12) & 63];
        if (rem == 2) *w++ = alpha[(v >> 6) & 63];
        else if (padded) *w++ = '=';
        if (padded) *w++ = '=';
    }
    return r;
}

static int b64_val(uint8_t c, bool url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == (url ? '-' : '+')) return 62;
    if (c == (url ? '_' : '/')) return 63;
    return -1;
}

static void corrupt(slang_error **err, const char *what, int64_t at) {
    if (!err) return;
    char buf[96];
    snprintf(buf, sizeof buf, "illegal %s data at input byte %lld", what, (long long)at);
    *err = slang_error_new_cstr(buf);
}

/* Decodes into an owned buffer (*outp, *outn). `need_pad`: the input length
 * (without '\r'/'\n') must be a multiple of 4 with '=' padding; otherwise
 * padding is optional, but if present it must complete the last quad.
 * Trailing bits of the last character are ignored (Go's non-strict mode).
 * Returns false (and sets *err) on bad input. */
static bool b64_decode(const uint8_t *p, int64_t n, bool url, bool need_pad,
                       uint8_t **outp, int64_t *outn, slang_error **err) {
    int64_t *pos = slang_alloc((n + 1) * (int64_t)sizeof(int64_t));
    uint8_t *o = slang_alloc(n / 4 * 3 + 3);
    int64_t m = 0;
    for (int64_t i = 0; i < n; i++)
        if (p[i] != '\r' && p[i] != '\n') pos[m++] = i;
    pos[m] = n;
    int64_t pads = 0;
    while (pads < m && pads < 3 && p[pos[m - 1 - pads]] == '=') pads++;
    int64_t d = m - pads, w = 0, bad = -1;
    for (int64_t i = 0; i < d && bad < 0; i++)
        if (b64_val(p[pos[i]], url) < 0) bad = pos[i];
    if (bad < 0) {
        if (pads > 2) bad = pos[d];
        else if ((need_pad || pads) && m % 4 != 0) bad = pos[d];
        else if (d % 4 == 1) bad = pos[d - 1];
        else if (pads && (d % 4) + pads != 4) bad = pos[d];
    }
    if (bad >= 0) {
        corrupt(err, "base64", bad);
        slang_free(pos);
        slang_free(o);
        return false;
    }
    uint32_t acc = 0;
    int q = 0;
    for (int64_t i = 0; i < d; i++) {
        acc = acc << 6 | (uint32_t)b64_val(p[pos[i]], url);
        if (++q == 4) {
            o[w++] = (uint8_t)(acc >> 16); o[w++] = (uint8_t)(acc >> 8); o[w++] = (uint8_t)acc;
            acc = 0;
            q = 0;
        }
    }
    if (q == 2) {
        o[w++] = (uint8_t)(acc >> 4);
    } else if (q == 3) {
        o[w++] = (uint8_t)(acc >> 10);
        o[w++] = (uint8_t)(acc >> 2);
    }
    slang_free(pos);
    *outp = o;
    *outn = w;
    return true;
}

/* ---- base64 API ---- */

slang_string *slang_encoding_base64_encode(const slang_slice *data) {
    return b64_encode(bbytes(data), blen(data), STD_ALPHA, true);
}
slang_string *slang_encoding_base64_encode_string(const slang_string *s) {
    return b64_encode(sbytes(s), slen(s), STD_ALPHA, true);
}
slang_string *slang_encoding_base64_encode_url(const slang_slice *data) {
    return b64_encode(bbytes(data), blen(data), URL_ALPHA, false);
}

static void decode_bytes(const slang_string *s, bool url, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    uint8_t *b;
    int64_t n;
    if (!b64_decode(sbytes(s), slen(s), url, !url, &b, &n, err)) return;
    slang_slice_from(out, 1, NULL, b, n);
    slang_free(b);
}

void slang_encoding_base64_decode(const slang_string *s, slang_slice *out, slang_error **err) {
    decode_bytes(s, false, out, err);
}
void slang_encoding_base64_decode_url(const slang_string *s, slang_slice *out, slang_error **err) {
    decode_bytes(s, true, out, err);
}
slang_string *slang_encoding_base64_decode_string(const slang_string *s, slang_error **err) {
    uint8_t *b;
    int64_t n;
    if (!b64_decode(sbytes(s), slen(s), false, true, &b, &n, err)) return NULL;
    slang_string *r = slang_string_new((const char *)b, n);
    slang_free(b);
    return r;
}

/* ---- hex ---- */

static slang_string *hex_encode(const uint8_t *p, int64_t n) {
    static const char hx[] = "0123456789abcdef";
    if (n > INT64_MAX / 2 - 1) slang_panic("hex: input too large");
    slang_string *r = slang__string_alloc(2 * n);
    for (int64_t i = 0; i < n; i++) {
        r->data[2 * i] = hx[p[i] >> 4];
        r->data[2 * i + 1] = hx[p[i] & 15];
    }
    return r;
}

static int hex_val(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Owned malloc'd buffer or NULL + *err. Go hex.DecodeString semantics. */
static uint8_t *hex_decode(const slang_string *s, int64_t *outn, slang_error **err) {
    int64_t n = slen(s);
    const uint8_t *p = sbytes(s);
    uint8_t *o = slang_alloc(n / 2 + 1);
    for (int64_t i = 0; i + 1 < n; i += 2) {
        int a = hex_val(p[i]), b = hex_val(p[i + 1]);
        if (a < 0 || b < 0) {
            if (err) {
                char buf[96];
                snprintf(buf, sizeof buf, "encoding/hex: invalid byte at offset %lld",
                         (long long)(a < 0 ? i : i + 1));
                *err = slang_error_new_cstr(buf);
            }
            slang_free(o);
            return NULL;
        }
        o[i / 2] = (uint8_t)(a << 4 | b);
    }
    if (n % 2) {
        if (err) *err = slang_error_new_cstr("encoding/hex: odd length hex string");
        slang_free(o);
        return NULL;
    }
    *outn = n / 2;
    return o;
}

slang_string *slang_encoding_hex_encode(const slang_slice *data) {
    return hex_encode(bbytes(data), blen(data));
}
slang_string *slang_encoding_hex_encode_string(const slang_string *s) {
    return hex_encode(sbytes(s), slen(s));
}
void slang_encoding_hex_decode(const slang_string *s, slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    int64_t n;
    uint8_t *b = hex_decode(s, &n, err);
    if (!b) return;
    slang_slice_from(out, 1, NULL, b, n);
    slang_free(b);
}
slang_string *slang_encoding_hex_decode_string(const slang_string *s, slang_error **err) {
    int64_t n;
    uint8_t *b = hex_decode(s, &n, err);
    if (!b) return NULL;
    slang_string *r = slang_string_new((const char *)b, n);
    slang_free(b);
    return r;
}
