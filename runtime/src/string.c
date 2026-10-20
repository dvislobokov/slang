/* string.c - immutable UTF-8 strings. NULL is the empty string. */
#include "rt_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const slang_type_info slang_type_string = { "string", 0, NULL };

static inline int64_t slen(const slang_string *s) { return s ? s->len : 0; }
static inline const char *sdata(const slang_string *s) { return s ? s->data : ""; }

slang_string *slang__string_alloc(int64_t len) {
    if (len < 0) slang__panicf("negative string length %lld", (long long)len);
    slang_string *s = slang_object_new(&slang_type_string,
                                       (int64_t)sizeof(slang_string) + len + 1);
    s->len = len;
    return s;
}

slang_string *slang_string_new(const char *bytes, int64_t len) {
    slang_string *s = slang__string_alloc(len);
    if (len > 0) memcpy(s->data, bytes, (size_t)len);
    return s;
}

slang_string *slang_string_from_cstr(const char *cstr) {
    if (!cstr) cstr = "";
    return slang_string_new(cstr, (int64_t)strlen(cstr));
}

slang_string *slang_string_literal(const char *bytes, int64_t len) {
    slang_string *s = slang_string_new(bytes, len);
    s->hdr.refcount = SLANG_RC_IMMORTAL;
    return s;
}

const char *slang_string_data(const slang_string *s) { return sdata(s); }
int64_t slang_string_len(const slang_string *s) { return slen(s); }

/* Decode one rune at p[0..n). Returns rune, sets *w (>=1). Invalid -> U+FFFD, w=1. */
static int32_t utf8_decode(const unsigned char *p, int64_t n, int64_t *w) {
    unsigned c = p[0];
    if (c < 0x80) { *w = 1; return (int32_t)c; }
    int need; uint32_t r, min;
    if ((c & 0xE0) == 0xC0) { need = 1; r = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { need = 2; r = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { need = 3; r = c & 0x07; min = 0x10000; }
    else { *w = 1; return 0xFFFD; }
    if (n < need + 1) { *w = 1; return 0xFFFD; }
    for (int i = 1; i <= need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *w = 1; return 0xFFFD; }
        r = (r << 6) | (p[i] & 0x3F);
    }
    if (r < min || r > 0x10FFFF || (r >= 0xD800 && r <= 0xDFFF)) { *w = 1; return 0xFFFD; }
    *w = need + 1;
    return (int32_t)r;
}

int slang__utf8_encode(char *buf, int32_t r) {
    uint32_t c = (uint32_t)r;
    if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
    if (c < 0x80) { buf[0] = (char)c; return 1; }
    if (c < 0x800) {
        buf[0] = (char)(0xC0 | (c >> 6)); buf[1] = (char)(0x80 | (c & 0x3F)); return 2;
    }
    if (c < 0x10000) {
        buf[0] = (char)(0xE0 | (c >> 12)); buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (c & 0x3F)); return 3;
    }
    buf[0] = (char)(0xF0 | (c >> 18)); buf[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((c >> 6) & 0x3F)); buf[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

int64_t slang_string_rune_count(const slang_string *s) {
    int64_t n = slen(s), count = 0;
    const unsigned char *p = (const unsigned char *)sdata(s);
    for (int64_t i = 0; i < n;) {
        int64_t w;
        utf8_decode(p + i, n - i, &w);
        i += w;
        count++;
    }
    return count;
}

int32_t slang_string_decode_rune(const slang_string *s, int64_t i, int64_t *width) {
    int64_t n = slen(s);
    if (i < 0 || i >= n) slang_panic_bounds(i, n);
    int64_t w;
    int32_t r = utf8_decode((const unsigned char *)sdata(s) + i, n - i, &w);
    if (width) *width = w;
    return r;
}

slang_string *slang_string_concat(const slang_string *a, const slang_string *b) {
    int64_t la = slen(a), lb = slen(b);
    slang_string *s = slang__string_alloc(la + lb);
    if (la) memcpy(s->data, a->data, (size_t)la);
    if (lb) memcpy(s->data + la, b->data, (size_t)lb);
    return s;
}

int32_t slang_string_compare(const slang_string *a, const slang_string *b) {
    int64_t la = slen(a), lb = slen(b);
    int64_t m = la < lb ? la : lb;
    int c = m ? memcmp(sdata(a), sdata(b), (size_t)m) : 0;
    if (c < 0) return -1;
    if (c > 0) return 1;
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

bool slang_string_eq(const slang_string *a, const slang_string *b) {
    if (a == b) return true;
    int64_t la = slen(a);
    if (la != slen(b)) return false;
    return la == 0 || memcmp(a->data, b->data, (size_t)la) == 0;
}

uint64_t slang_hash_bytes(const void *p, int64_t n) {
    const unsigned char *b = p;
    uint64_t h = 1469598103934665603ULL;
    for (int64_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

uint64_t slang_string_hash(const slang_string *s) { return slang_hash_bytes(sdata(s), slen(s)); }

slang_string *slang_string_slice(const slang_string *s, int64_t lo, int64_t hi) {
    int64_t n = slen(s);
    if (lo < 0 || hi < lo || hi > n) slang_panic_slice_bounds(lo, hi, n);
    return slang_string_new(sdata(s) + lo, hi - lo);
}

uint8_t slang_string_byte_at(const slang_string *s, int64_t i) {
    int64_t n = slen(s);
    if (i < 0 || i >= n) slang_panic_bounds(i, n);
    return (uint8_t)s->data[i];
}

int64_t slang_string_index(const slang_string *s, const slang_string *sub) {
    int64_t n = slen(s), m = slen(sub);
    if (m == 0) return 0;
    if (m > n) return -1;
    const char *hs = sdata(s), *nd = sdata(sub);
    for (int64_t i = 0; i + m <= n; i++) {
        if (hs[i] == nd[0] && memcmp(hs + i, nd, (size_t)m) == 0) return i;
    }
    return -1;
}

bool slang_string_contains(const slang_string *s, const slang_string *sub) {
    return slang_string_index(s, sub) >= 0;
}

bool slang_string_has_prefix(const slang_string *s, const slang_string *p) {
    int64_t m = slen(p);
    return m <= slen(s) && (m == 0 || memcmp(sdata(s), sdata(p), (size_t)m) == 0);
}

bool slang_string_has_suffix(const slang_string *s, const slang_string *p) {
    int64_t n = slen(s), m = slen(p);
    return m <= n && (m == 0 || memcmp(sdata(s) + n - m, sdata(p), (size_t)m) == 0);
}

/* ---- conversions ---- */

slang_string *slang_string_from_int(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%" PRId64, v);
    return slang_string_new(buf, n);
}

slang_string *slang_string_from_uint(uint64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%" PRIu64, v);
    return slang_string_new(buf, n);
}

int slang__format_float(char *buf, size_t cap, double v) {
    if (isnan(v)) return snprintf(buf, cap, "NaN");
    if (isinf(v)) return snprintf(buf, cap, v > 0 ? "+Inf" : "-Inf");
    int n = 0;
    for (int prec = 1; prec <= 17; prec++) {
        n = snprintf(buf, cap, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    return n;
}

slang_string *slang_string_from_float(double v) {
    char buf[64];
    int n = slang__format_float(buf, sizeof buf, v);
    return slang_string_new(buf, n);
}

slang_string *slang_string_from_bool(bool v) {
    return v ? slang_string_new("true", 4) : slang_string_new("false", 5);
}

slang_string *slang_string_from_rune(int32_t r) {
    char buf[4];
    int n = slang__utf8_encode(buf, r);
    return slang_string_new(buf, n);
}

static void set_err(slang_error **err, const char *fmt, const slang_string *s) {
    if (!err) return;
    char buf[256];
    snprintf(buf, sizeof buf, fmt, sdata(s));
    *err = slang_error_new_cstr(buf);
}

int64_t slang_string_to_int(const slang_string *s, slang_error **err) {
    if (err) *err = NULL;
    const char *p = sdata(s);
    int64_t n = slen(s);
    if (n == 0) { set_err(err, "invalid integer: \"%s\"", s); return 0; }
    /* reject leading whitespace / embedded NULs that strtoll would accept */
    if ((int64_t)strlen(p) != n || p[0] == ' ' || p[0] == '\t' || p[0] == '\n') {
        set_err(err, "invalid integer: \"%s\"", s); return 0;
    }
    errno = 0;
    char *end;
    long long v = strtoll(p, &end, 10);
    if (end != p + n) { set_err(err, "invalid integer: \"%s\"", s); return 0; }
    if (errno == ERANGE) { set_err(err, "integer out of range: \"%s\"", s); return 0; }
    return (int64_t)v;
}

double slang_string_to_float(const slang_string *s, slang_error **err) {
    if (err) *err = NULL;
    const char *p = sdata(s);
    int64_t n = slen(s);
    if (n == 0 || (int64_t)strlen(p) != n || p[0] == ' ' || p[0] == '\t' || p[0] == '\n') {
        set_err(err, "invalid float: \"%s\"", s); return 0;
    }
    errno = 0;
    char *end;
    double v = strtod(p, &end);
    if (end != p + n) { set_err(err, "invalid float: \"%s\"", s); return 0; }
    if (errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL)) {
        set_err(err, "float out of range: \"%s\"", s); return 0;
    }
    return v;
}
