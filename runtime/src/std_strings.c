/* std_strings.c - std.strings (slang_strings_*). Semantics follow Go's strings
 * package unless noted in ABI.md section 14. All string arguments are borrowed
 * and may be NULL (== ""); every returned string is owned (+1) and non-NULL.
 * A result that equals the argument may be the argument itself, retained. */
#include "rt_internal.h"

#include <string.h>

static inline int64_t slen(const slang_string *s) { return s ? s->len : 0; }
static inline const char *sdata(const slang_string *s) { return s ? s->data : ""; }

/* Owned copy of s (or s retained when non-NULL; strings are immutable). */
static slang_string *same(const slang_string *s) {
    if (!s) return slang__string_alloc(0);
    slang_retain((void *)s);
    return (slang_string *)s;
}

/* Byte width of the rune at s[i] (1 for an invalid sequence). */
static int64_t rune_width(const slang_string *s, int64_t i) {
    int64_t w;
    slang_string_decode_rune(s, i, &w);
    return w;
}

static int64_t count_nonoverlapping(const slang_string *s, const slang_string *sub) {
    int64_t n = slen(s), m = slen(sub), c = 0;
    const char *p = sdata(s), *q = sdata(sub), *end = p + n;
    for (const char *f; (f = slang__memmem(p, end - p, q, m)) != NULL; p = f + m) c++;
    return c;
}

/* ---- Split / Join ---- */

void slang_strings_split(const slang_string *s, const slang_string *sep, slang_slice *out) {
    int64_t n = slen(s), m = slen(sep);
    const char *p = sdata(s);
    if (m == 0) {
        /* Go: split after each UTF-8 sequence; "" -> empty slice. */
        int64_t count = slang_string_rune_count(s);
        slang_slice_new_ops(out, 8, count, count, &slang_ops_ref);
        slang_string **slots = out->data;
        for (int64_t i = 0, k = 0; i < n; k++) {
            int64_t w = rune_width(s, i);
            slots[k] = slang_string_new(p + i, w);
            i += w;
        }
        return;
    }
    /* Count first so the result slice is allocated once, then cut. */
    int64_t count = count_nonoverlapping(s, sep) + 1;
    slang_slice_new_ops(out, 8, count, count, &slang_ops_ref);
    slang_string **slots = out->data;
    const char *q = sdata(sep), *start = p, *end = p + n;
    int64_t k = 0;
    for (const char *f; k < count - 1 && (f = slang__memmem(start, end - start, q, m)) != NULL;
         start = f + m)
        slots[k++] = slang_string_new(start, f - start);
    slots[k] = slang_string_new(start, end - start);
}

slang_string *slang_strings_join(const slang_slice *parts, const slang_string *sep) {
    int64_t cnt = parts ? parts->len : 0;
    if (cnt == 0) return slang__string_alloc(0);
    slang_string *const *el = parts->data;
    int64_t m = slen(sep), total = m * (cnt - 1);
    for (int64_t i = 0; i < cnt; i++) total += slen(el[i]);
    slang_string *r = slang__string_alloc_uninit(total);
    char *w = r->data;
    for (int64_t i = 0; i < cnt; i++) {
        if (i > 0 && m) { memcpy(w, sdata(sep), (size_t)m); w += m; }
        int64_t l = slen(el[i]);
        if (l) { memcpy(w, sdata(el[i]), (size_t)l); w += l; }
    }
    return r;
}

/* ---- Trim (Go TrimSpace: unicode.IsSpace) ---- */

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

slang_string *slang_strings_trim(const slang_string *s) {
    int64_t n = slen(s), lo = 0, hi = n;
    const unsigned char *p = (const unsigned char *)sdata(s);
    while (lo < hi) {
        int64_t w;
        int32_t r = slang_string_decode_rune(s, lo, &w);
        if (!is_space(r)) break;
        lo += w;
    }
    while (hi > lo) {
        /* start of the last rune: step back over up to 3 continuation bytes */
        int64_t st = hi - 1;
        while (st > lo && hi - st < 4 && (p[st] & 0xC0) == 0x80) st--;
        int64_t w;
        int32_t r = slang_string_decode_rune(s, st, &w);
        if (st + w != hi) { st = hi - 1; r = 0xFFFD; }  /* invalid tail byte */
        if (!is_space(r)) break;
        hi = st;
    }
    if (lo == 0 && hi == n) return same(s);
    return slang_string_new((const char *)p + lo, hi - lo);
}

/* ---- predicates / search (thin wrappers) ---- */

bool slang_strings_contains(const slang_string *s, const slang_string *substr) {
    return slang_string_contains(s, substr);
}
bool slang_strings_has_prefix(const slang_string *s, const slang_string *prefix) {
    return slang_string_has_prefix(s, prefix);
}
bool slang_strings_has_suffix(const slang_string *s, const slang_string *suffix) {
    return slang_string_has_suffix(s, suffix);
}
int64_t slang_strings_index(const slang_string *s, const slang_string *substr) {
    return slang_string_index(s, substr);
}

/* ---- Replace (all occurrences, Go strings.ReplaceAll) ---- */

slang_string *slang_strings_replace(const slang_string *s, const slang_string *old,
                                    const slang_string *new_) {
    int64_t n = slen(s), mo = slen(old), mn = slen(new_);
    const char *p = sdata(s), *po = sdata(old), *pn = sdata(new_);
    if (slang_string_eq(old, new_)) return same(s);
    if (mo == 0) {
        /* Go: insert new_ at the start and after every UTF-8 sequence. */
        int64_t runes = slang_string_rune_count(s);
        slang_string *r = slang__string_alloc(n + mn * (runes + 1));
        char *w = r->data;
        memcpy(w, pn, (size_t)mn); w += mn;
        for (int64_t i = 0; i < n;) {
            int64_t rw = rune_width(s, i);
            memcpy(w, p + i, (size_t)rw); w += rw; i += rw;
            memcpy(w, pn, (size_t)mn); w += mn;
        }
        return r;
    }
    int64_t c = count_nonoverlapping(s, old);
    if (c == 0) return same(s);
    slang_string *r = slang__string_alloc_uninit(n + c * (mn - mo));
    char *w = r->data;
    const char *start = p, *end = p + n;
    for (const char *f; (f = slang__memmem(start, end - start, po, mo)) != NULL; start = f + mo) {
        memcpy(w, start, (size_t)(f - start)); w += f - start;
        if (mn) memcpy(w, pn, (size_t)mn);
        w += mn;
    }
    memcpy(w, start, (size_t)(end - start));
    return r;
}

/* ---- ToUpper / ToLower ----
 * Simple (1:1) case mapping for ASCII, Latin-1, Latin Extended-A, Greek and
 * Cyrillic (U+0400..U+04BF). Other code points and invalid bytes are copied
 * unchanged. No mapping in these ranges grows the UTF-8 length. */

static int32_t to_upper(int32_t r) {
    if (r >= 'a' && r <= 'z') return r - 32;
    if (r < 0x80) return r;
    if (r == 0xB5) return 0x39C;
    if (r >= 0xE0 && r <= 0xFE && r != 0xF7) return r - 0x20;
    if (r == 0xFF) return 0x178;
    if (r >= 0x100 && r <= 0x12F) return r & 1 ? r - 1 : r;
    if (r == 0x131) return 'I';
    if (r >= 0x132 && r <= 0x137) return r & 1 ? r - 1 : r;
    if (r >= 0x139 && r <= 0x148) return r & 1 ? r : r - 1;
    if (r >= 0x14A && r <= 0x177) return r & 1 ? r - 1 : r;
    if (r >= 0x179 && r <= 0x17E) return r & 1 ? r : r - 1;
    if (r == 0x17F) return 'S';
    if (r == 0x3AC) return 0x386;
    if (r >= 0x3AD && r <= 0x3AF) return r - 37;
    if (r >= 0x3B1 && r <= 0x3CB) return r == 0x3C2 ? 0x3A3 : r - 0x20;
    if (r == 0x3CC) return 0x38C;
    if (r >= 0x3CD && r <= 0x3CE) return r - 63;
    if (r >= 0x430 && r <= 0x44F) return r - 0x20;
    if (r >= 0x450 && r <= 0x45F) return r - 0x50;
    if ((r >= 0x460 && r <= 0x481) || (r >= 0x48A && r <= 0x4BF)) return r & 1 ? r - 1 : r;
    return r;
}

static int32_t to_lower(int32_t r) {
    if (r >= 'A' && r <= 'Z') return r + 32;
    if (r < 0x80) return r;
    if (r >= 0xC0 && r <= 0xDE && r != 0xD7) return r + 0x20;
    if (r == 0x178) return 0xFF;
    if (r >= 0x100 && r <= 0x12F) return r & 1 ? r : r + 1;
    if (r == 0x130) return 'i';
    if (r >= 0x132 && r <= 0x137) return r & 1 ? r : r + 1;
    if (r >= 0x139 && r <= 0x148) return r & 1 ? r + 1 : r;
    if (r >= 0x14A && r <= 0x177) return r & 1 ? r : r + 1;
    if (r >= 0x179 && r <= 0x17E) return r & 1 ? r + 1 : r;
    if (r == 0x386) return 0x3AC;
    if (r >= 0x388 && r <= 0x38A) return r + 37;
    if (r == 0x38C) return 0x3CC;
    if (r >= 0x38E && r <= 0x38F) return r + 63;
    if ((r >= 0x391 && r <= 0x3A1) || (r >= 0x3A3 && r <= 0x3AB)) return r + 0x20;
    if (r >= 0x400 && r <= 0x40F) return r + 0x50;
    if (r >= 0x410 && r <= 0x42F) return r + 0x20;
    if ((r >= 0x460 && r <= 0x481) || (r >= 0x48A && r <= 0x4BF)) return r & 1 ? r : r + 1;
    return r;
}

/* ASCII case change of 8 bytes at once (all bytes < 0x80): flips bit 5 of
 * every byte in [lo, hi]. */
static inline uint64_t swar_case(uint64_t x, unsigned char lo, unsigned char hi) {
    const uint64_t ones = 0x0101010101010101ULL;
    uint64_t ge = x + ones * (0x80 - lo);          /* bit 7 set if byte >= lo */
    uint64_t gt = x + ones * (0x7F - hi);          /* bit 7 set if byte > hi */
    uint64_t in = (ge & ~gt) & (ones * 0x80);
    return x ^ (in >> 2);
}

static slang_string *map_case(const slang_string *s, int32_t (*f)(int32_t), bool upper) {
    int64_t n = slen(s);
    const char *p = sdata(s);
    unsigned char lo = upper ? 'a' : 'A', hi = upper ? 'z' : 'Z';
    /* No mapping grows the UTF-8 length, so n bytes always suffice; a
     * shorter result just leaves slack in the allocation. */
    slang_string *res = slang__string_alloc_uninit(n);
    char *buf = res->data;
    int64_t len = 0;
    for (int64_t i = 0; i < n;) {
        /* ASCII fast path, 8 bytes per step. */
        while (i + 8 <= n) {
            uint64_t x;
            memcpy(&x, p + i, 8);
            if (x & 0x8080808080808080ULL) break;
            x = swar_case(x, lo, hi);
            memcpy(buf + len, &x, 8);
            i += 8;
            len += 8;
        }
        if (i >= n) break;
        unsigned char c = (unsigned char)p[i];
        if (c < 0x80) { buf[len++] = (char)f(c); i++; continue; }
        int64_t w;
        int32_t r = slang_string_decode_rune(s, i, &w);
        int32_t m = (r == 0xFFFD && w == 1) ? r : f(r);
        if (m == r) {  /* unchanged: copy the original bytes (keeps invalid bytes) */
            memcpy(buf + len, p + i, (size_t)w);
            len += w;
        } else {
            len += slang__utf8_encode(buf + len, m);
        }
        i += w;
    }
    res->len = len;
    buf[len] = 0;
    return res;
}

slang_string *slang_strings_to_upper(const slang_string *s) { return map_case(s, to_upper, true); }
slang_string *slang_strings_to_lower(const slang_string *s) { return map_case(s, to_lower, false); }

/* ---- Repeat ---- */

slang_string *slang_strings_repeat(const slang_string *s, int64_t count) {
    if (count < 0) slang_panic("strings: negative Repeat count");
    int64_t n = slen(s);
    if (count == 0 || n == 0) return slang__string_alloc(0);
    if (n > (INT64_MAX - 64) / count) slang_panic("strings: Repeat output length overflow");
    slang_string *r = slang__string_alloc(n * count);
    for (int64_t i = 0; i < count; i++) memcpy(r->data + i * n, s->data, (size_t)n);
    return r;
}

/* ---- conversions (T! -> value + slang_error ** out-param) ---- */

int64_t slang_strings_to_int(const slang_string *s, slang_error **err) {
    return slang_string_to_int(s, err);
}

double slang_strings_to_float(const slang_string *s, slang_error **err) {
    return slang_string_to_float(s, err);
}
