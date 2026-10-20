/* std_strings_ext.c - more std.strings functions (slang_strings_*), Go
 * semantics unless ABI.md section 14 says otherwise. Offsets and counts are
 * in bytes except where "runes" is stated; invalid UTF-8 bytes count as one
 * rune each (decoded as U+FFFD). */
#include "rt_internal.h"

#include <string.h>

static inline int64_t slen(const slang_string *s) { return s ? s->len : 0; }
static inline const char *sdata(const slang_string *s) { return s ? s->data : ""; }

static slang_string *same(const slang_string *s) {
    if (!s) return slang__string_alloc(0);
    slang_retain((void *)s);
    return (slang_string *)s;
}

static slang_string *sub(const slang_string *s, int64_t lo, int64_t hi) {
    if (lo == 0 && hi == slen(s)) return same(s);
    return slang_string_new(sdata(s) + lo, hi - lo);
}

static bool is_space(int32_t r) {
    switch (r) {
    case '\t': case '\n': case '\v': case '\f': case '\r': case ' ':
    case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return r >= 0x2000 && r <= 0x200A;
    }
}

/* Start offset of the rune that ends at hi (hi > 0). */
static int64_t rune_start_before(const slang_string *s, int64_t hi, int32_t *r) {
    const unsigned char *p = (const unsigned char *)sdata(s);
    int64_t st = hi - 1;
    while (st > 0 && hi - st < 4 && (p[st] & 0xC0) == 0x80) st--;
    int64_t w;
    int32_t c = slang_string_decode_rune(s, st, &w);
    if (st + w != hi) { st = hi - 1; c = 0xFFFD; }
    *r = c;
    return st;
}

static void push(slang_slice *out, const char *p, int64_t n) {
    slang_string *x = slang_string_new(p, n);
    slang_slice_append(out, 8, &slang_ops_ref, &x);
    slang_release(x);
}

/* ---- Fields ---- */

void slang_strings_fields(const slang_string *s, slang_slice *out) {
    slang_slice_new_ops(out, 8, 0, 4, &slang_ops_ref);
    int64_t n = slen(s), start = -1;
    for (int64_t i = 0; i < n;) {
        int64_t w;
        int32_t r = slang_string_decode_rune(s, i, &w);
        bool sp = is_space(r) && !(r == 0xFFFD && w == 1);
        if (sp && start >= 0) { push(out, sdata(s) + start, i - start); start = -1; }
        if (!sp && start < 0) start = i;
        i += w;
    }
    if (start >= 0) push(out, sdata(s) + start, n - start);
}

/* ---- Count / LastIndex ---- */

int64_t slang_strings_count(const slang_string *s, const slang_string *substr) {
    int64_t n = slen(s), m = slen(substr), c = 0;
    if (m == 0) return slang_string_rune_count(s) + 1;
    const char *p = sdata(s), *q = sdata(substr);
    for (int64_t i = 0; i + m <= n;) {
        if (memcmp(p + i, q, (size_t)m) == 0) { c++; i += m; } else i++;
    }
    return c;
}

int64_t slang_strings_last_index(const slang_string *s, const slang_string *substr) {
    int64_t n = slen(s), m = slen(substr);
    if (m > n) return -1;
    const char *p = sdata(s), *q = sdata(substr);
    for (int64_t i = n - m; i >= 0; i--)
        if (memcmp(p + i, q, (size_t)m) == 0) return i;
    return -1;
}

/* ---- trimming ---- */

slang_string *slang_strings_trim_prefix(const slang_string *s, const slang_string *prefix) {
    if (slen(prefix) && slang_string_has_prefix(s, prefix)) return sub(s, slen(prefix), slen(s));
    return same(s);
}

slang_string *slang_strings_trim_suffix(const slang_string *s, const slang_string *suffix) {
    if (slen(suffix) && slang_string_has_suffix(s, suffix)) return sub(s, 0, slen(s) - slen(suffix));
    return same(s);
}

/* Is rune r (of width w at s) in cutset? Invalid bytes match only the same byte. */
static bool in_cutset(const slang_string *cutset, int32_t r, bool invalid, unsigned char byte) {
    int64_t n = slen(cutset);
    for (int64_t i = 0; i < n;) {
        int64_t w;
        int32_t c = slang_string_decode_rune(cutset, i, &w);
        bool cinv = c == 0xFFFD && w == 1;
        if (invalid ? (cinv && (unsigned char)sdata(cutset)[i] == byte) : (!cinv && c == r)) return true;
        i += w;
    }
    return false;
}

slang_string *slang_strings_trim_left(const slang_string *s, const slang_string *cutset) {
    int64_t n = slen(s), lo = 0;
    while (lo < n) {
        int64_t w;
        int32_t r = slang_string_decode_rune(s, lo, &w);
        if (!in_cutset(cutset, r, r == 0xFFFD && w == 1, (unsigned char)sdata(s)[lo])) break;
        lo += w;
    }
    return sub(s, lo, n);
}

slang_string *slang_strings_trim_right(const slang_string *s, const slang_string *cutset) {
    int64_t hi = slen(s);
    while (hi > 0) {
        int32_t r;
        int64_t st = rune_start_before(s, hi, &r);
        bool inv = r == 0xFFFD && hi - st == 1;
        if (!in_cutset(cutset, r, inv, (unsigned char)sdata(s)[st])) break;
        hi = st;
    }
    return sub(s, 0, hi);
}

/* ---- SplitN ---- */

void slang_strings_split_n(const slang_string *s, const slang_string *sep, int64_t n,
                           slang_slice *out) {
    if (n < 0) { slang_strings_split(s, sep, out); return; }
    slang_slice_new_ops(out, 8, 0, n < 16 ? n : 16, &slang_ops_ref);
    if (n == 0) return;
    int64_t len = slen(s), m = slen(sep), start = 0, i = 0;
    const char *p = sdata(s), *q = sdata(sep);
    if (m == 0) {
        while (i < len && out->len < n - 1) {
            int64_t w;
            slang_string_decode_rune(s, i, &w);
            push(out, p + i, w);
            i += w;
        }
        if (i < len) push(out, p + i, len - i);
        return;
    }
    while (out->len < n - 1 && i + m <= len) {
        if (memcmp(p + i, q, (size_t)m) == 0) {
            push(out, p + start, i - start);
            i += m;
            start = i;
        } else {
            i++;
        }
    }
    push(out, p + start, len - start);
}

/* ---- EqualFold / Compare ---- */

bool slang_strings_equal_fold(const slang_string *a, const slang_string *b) {
    /* simple folding: lower(upper(x)) maps ς, σ, Σ (and ſ, s, S) together */
    slang_string *ua = slang_strings_to_upper(a), *ub = slang_strings_to_upper(b);
    slang_string *la = slang_strings_to_lower(ua), *lb = slang_strings_to_lower(ub);
    bool eq = slang_string_eq(la, lb);
    slang_release(ua); slang_release(ub); slang_release(la); slang_release(lb);
    return eq;
}

int64_t slang_strings_compare(const slang_string *a, const slang_string *b) {
    return slang_string_compare(a, b);
}

/* ---- Reverse / Pad / Title ---- */

slang_string *slang_strings_reverse(const slang_string *s) {
    int64_t n = slen(s);
    slang_string *r = slang__string_alloc(n);
    for (int64_t i = 0; i < n;) {
        int64_t w;
        slang_string_decode_rune(s, i, &w);
        memcpy(r->data + (n - i - w), sdata(s) + i, (size_t)w);
        i += w;
    }
    return r;
}

static slang_string *pad(const slang_string *s, int64_t width, const slang_string *padding, bool left) {
    int64_t have = slang_string_rune_count(s);
    if (have >= width || slen(padding) == 0) return same(s);
    int64_t need = width - have;
    /* `need` runes of padding cycled from `padding`: at most 4 bytes each */
    if (need > (INT64_MAX / 8)) slang_panic("strings: Pad width overflow");
    char *buf = slang_alloc(need * 4 + slen(s) + 1);
    int64_t len = 0, pi = 0, pn = slen(padding);
    if (!left) { memcpy(buf, sdata(s), (size_t)slen(s)); len = slen(s); }
    for (int64_t k = 0; k < need; k++) {
        int64_t w;
        slang_string_decode_rune(padding, pi, &w);
        memcpy(buf + len, sdata(padding) + pi, (size_t)w);
        len += w;
        pi += w;
        if (pi >= pn) pi = 0;
    }
    if (left) { memcpy(buf + len, sdata(s), (size_t)slen(s)); len += slen(s); }
    slang_string *r = slang_string_new(buf, len);
    slang_free(buf);
    return r;
}

slang_string *slang_strings_pad_left(const slang_string *s, int64_t width, const slang_string *padding) {
    return pad(s, width, padding, true);
}

slang_string *slang_strings_pad_right(const slang_string *s, int64_t width, const slang_string *padding) {
    return pad(s, width, padding, false);
}

slang_string *slang_strings_title(const slang_string *s) {
    int64_t n = slen(s);
    char *buf = slang_alloc(n + 1);
    int64_t len = 0;
    bool word_start = true;
    for (int64_t i = 0; i < n;) {
        int64_t w;
        int32_t r = slang_string_decode_rune(s, i, &w);
        bool sp = is_space(r) && !(r == 0xFFFD && w == 1);
        if (word_start && !sp) {
            slang_string *one = slang_string_new(sdata(s) + i, w);
            slang_string *up = slang_strings_to_upper(one);   /* never grows (ABI 14.2) */
            memcpy(buf + len, up->data, (size_t)up->len);
            len += up->len;
            slang_release(up);
            slang_release(one);
        } else {
            memcpy(buf + len, sdata(s) + i, (size_t)w);
            len += w;
        }
        word_start = sp;
        i += w;
    }
    slang_string *res = slang_string_new(buf, len);
    slang_free(buf);
    return res;
}

/* ---- runes / bytes ---- */

void slang_strings_runes(const slang_string *s, slang_slice *out) {
    int64_t cnt = slang_string_rune_count(s), n = slen(s);
    slang_slice_new(out, 4, cnt, cnt);
    int32_t *rs = out->data;
    for (int64_t i = 0, k = 0; i < n; k++) {
        int64_t w;
        rs[k] = slang_string_decode_rune(s, i, &w);
        i += w;
    }
}

slang_string *slang_strings_from_runes(const slang_slice *runes) {
    int64_t cnt = runes ? runes->len : 0;
    if (cnt == 0) return slang__string_alloc(0);
    const int32_t *rs = runes->data;
    char *buf = slang_alloc(cnt * 4 + 1);
    int64_t len = 0;
    for (int64_t i = 0; i < cnt; i++) {
        int32_t r = rs[i];
        if (r < 0 || r > 0x10FFFF || (r >= 0xD800 && r <= 0xDFFF)) r = 0xFFFD;
        len += slang__utf8_encode(buf + len, r);
    }
    slang_string *res = slang_string_new(buf, len);
    slang_free(buf);
    return res;
}

void slang_strings_bytes(const slang_string *s, slang_slice *out) { slang_string_to_bytes(out, s); }

slang_string *slang_strings_from_bytes(const slang_slice *bytes) {
    if (!bytes || bytes->len == 0) return slang__string_alloc(0);
    return slang_string_from_bytes(bytes);
}
