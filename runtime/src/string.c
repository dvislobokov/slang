/* string.c - immutable UTF-8 strings. NULL is the empty string. */
#include "rt_internal.h"

#include <ctype.h>
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

slang_string *slang__string_alloc_uninit(int64_t len) {
    if (len < 0) slang__panicf("negative string length %lld", (long long)len);
    slang_string *s = (slang_string *)slang__object_new_uninit(
        &slang_type_string, (int64_t)sizeof(slang_string) + len + 1);
    s->len = len;
    s->data[len] = 0;
    return s;
}

slang_string *slang_string_new(const char *bytes, int64_t len) {
    slang_string *s = slang__string_alloc_uninit(len);
    if (len > 0) memcpy(s->data, bytes, (size_t)len);
    return s;
}

slang_string *slang_string_from_cstr(const char *cstr) {
    if (!cstr) cstr = "";
    return slang_string_new(cstr, (int64_t)strlen(cstr));
}

slang_string *slang_string_literal(const char *bytes, int64_t len) {
    slang_string *s = slang_string_new(bytes, len);
    slang__object_immortalize(&s->hdr);
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
    slang_string *s = slang__string_alloc_uninit(la + lb);
    if (la) memcpy(s->data, a->data, (size_t)la);
    if (lb) memcpy(s->data + la, b->data, (size_t)lb);
    return s;
}

slang_string *slang_string_append(slang_string *a, const slang_string *b) {
    int64_t la = slen(a), lb = slen(b);
    if (lb == 0) return a;
    if (!a || __atomic_load_n(&a->hdr.refcount, __ATOMIC_ACQUIRE) != 1) {
        /* Shared, immortal or empty: copy. */
        slang_string *s = slang_string_concat(a, b);
        slang_release(a);
        return s;
    }
    /* `a` is uniquely referenced by the caller: extend it in place, with
     * geometric growth so that a loop of `s += x` is linear. */
    int64_t need = (int64_t)sizeof(slang_string) + la + lb + 1;
    int self = (b == a);
    if (slang__object_capacity(&a->hdr) < need) {
        int64_t cap = need + need / 2 + 16;
        a = (slang_string *)slang__object_resize(&a->hdr, cap);
    }
    memcpy(a->data + la, self ? a->data : b->data, (size_t)lb);
    a->len = la + lb;
    a->data[a->len] = 0;
    return a;
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

const char *slang__memmem(const char *hay, int64_t n, const char *needle, int64_t m) {
    if (m > n) return NULL;
    /* memchr for the first byte (vectorised in libc), then verify. */
    const char *p = hay, *last = hay + (n - m);
    unsigned char c0 = (unsigned char)needle[0];
    while (p <= last) {
        p = memchr(p, c0, (size_t)(last - p + 1));
        if (!p) return NULL;
        if (memcmp(p + 1, needle + 1, (size_t)(m - 1)) == 0) return p;
        p++;
    }
    return NULL;
}

int64_t slang_string_index(const slang_string *s, const slang_string *sub) {
    int64_t n = slen(s), m = slen(sub);
    if (m == 0) return 0;
    const char *hs = sdata(s);
    const char *f = slang__memmem(hs, n, sdata(sub), m);
    return f ? (int64_t)(f - hs) : -1;
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

/* Decimal digits of v at the end of buf[0..32); returns the start. */
static char *utoa_end(char *end, uint64_t v) {
    char *p = end;
    do { *--p = (char)('0' + v % 10); v /= 10; } while (v);
    return p;
}

slang_string *slang_string_from_int(int64_t v) {
    char buf[32];
    char *end = buf + sizeof buf;
    char *p = utoa_end(end, v < 0 ? 0 - (uint64_t)v : (uint64_t)v);
    if (v < 0) *--p = '-';
    return slang_string_new(p, end - p);
}

slang_string *slang_string_from_uint(uint64_t v) {
    char buf[32];
    char *end = buf + sizeof buf;
    char *p = utoa_end(end, v);
    return slang_string_new(p, end - p);
}

/* Fast exact path of slang__format_float for values printed in fixed
 * notation with few digits (1e-4 <= |v| < 1e6, at most 15 significant
 * digits): finds the smallest k such that m = round(v * 10^k) parses back
 * to v. That m is the shortest round-trip digit string, i.e. exactly what
 * the %.*e search below produces. Returns the length, or -1 to fall back. */
static int format_float_fast(char *buf, size_t cap, double v) {
    static const double pow10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8,
                                    1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16,
                                    1e17, 1e18, 1e19, 1e20 };
    double a = fabs(v);
    if (!(a >= 1e-4 && a < 1e6) || cap < 32) return -1;
    for (int k = 0; k <= 20; k++) {
        double p = pow10[k];
        double m = rint(a * p);
        if (m >= 1e15) return -1;            /* > 15 digits: use the general path */
        if (m / p != a) continue;            /* exact: m, p < 2^53, one rounding */
        /* m must be the correctly rounded k-decimal value of a, not a
         * neighbour that happens to round-trip: check the exact residual. */
        double r = fma(a, p, -m);
        if (fabs(r) >= 0.4999) return -1;
        if (m >= 1e6 * p) return -1;         /* rounded up to 1e6: exponent form */
        char dig[32];
        char *end = dig + sizeof dig;
        char *d = utoa_end(end, (uint64_t)m);
        int nd = (int)(end - d);
        size_t n = 0;
        if (v < 0) buf[n++] = '-';
        if (nd <= k) {                       /* 0.000ddd */
            buf[n++] = '0';
            buf[n++] = '.';
            for (int i = nd; i < k; i++) buf[n++] = '0';
            memcpy(buf + n, d, (size_t)nd);
            n += (size_t)nd;
        } else {
            memcpy(buf + n, d, (size_t)(nd - k));
            n += (size_t)(nd - k);
            if (k > 0) {
                buf[n++] = '.';
                memcpy(buf + n, d + nd - k, (size_t)k);
                n += (size_t)k;
            }
        }
        buf[n] = 0;
        return (int)n;
    }
    return -1;
}

int slang__format_float(char *buf, size_t cap, double v) {
    if (isnan(v)) return snprintf(buf, cap, "NaN");
    if (isinf(v)) return snprintf(buf, cap, v > 0 ? "+Inf" : "-Inf");
    if (v == 0) return snprintf(buf, cap, signbit(v) ? "-0" : "0");
    int fast = format_float_fast(buf, cap, v);
    if (fast >= 0) return fast;

    /* Shortest precision that round-trips, in %e form to learn the exponent. */
    char tmp[64];
    int prec = 1;
    for (; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*e", prec - 1, v);
        if (strtod(tmp, NULL) == v) break;
    }
    /* tmp looks like "-d.ddde+XX"; extract exponent and digits. */
    char *epos = strchr(tmp, 'e');
    int exp = atoi(epos + 1);
    char digits[32];
    int nd = 0;
    for (char *c = tmp; c < epos; c++) {
        if (*c >= '0' && *c <= '9') digits[nd++] = *c;
    }
    digits[nd] = 0;
    bool neg = v < 0;

    /* Go %v (strconv 'g', shortest precision): exponent form when
       exp < -4 || exp >= 6, else fixed form without trailing zeros. */
    if (exp < -4 || exp >= 6) {
        if (nd > 1)
            return snprintf(buf, cap, "%s%c.%se%c%02d", neg ? "-" : "", digits[0], digits + 1,
                            exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
        return snprintf(buf, cap, "%s%ce%c%02d", neg ? "-" : "", digits[0],
                        exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
    }
    size_t n = 0;
    if (neg && n < cap) buf[n++] = '-';
    if (exp < 0) {
        /* 0.000ddd */
        n += (size_t)snprintf(buf + n, cap - n, "0.");
        for (int i = 0; i < -exp - 1 && n < cap; i++) buf[n++] = '0';
        n += (size_t)snprintf(buf + n, cap - n, "%s", digits);
    } else {
        int int_len = exp + 1;
        if (nd <= int_len) {
            n += (size_t)snprintf(buf + n, cap - n, "%s", digits);
            for (int i = nd; i < int_len && n < cap; i++) buf[n++] = '0';
            if (n < cap) buf[n] = 0;
        } else {
            n += (size_t)snprintf(buf + n, cap - n, "%.*s.%s", int_len, digits, digits + int_len);
        }
    }
    return (int)n;
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
    if ((int64_t)strlen(p) != n || isspace((unsigned char)p[0])) {
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
    if (n == 0 || (int64_t)strlen(p) != n || isspace((unsigned char)p[0])) {
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
