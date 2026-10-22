/* std_regex.c - the C half of regex (runtime/ABI.md 14.5). The engine
 * (parser, compiler, Pike VM, lazy DFA) is Zell code in
 * compiler/src/sema/stdlib_src/regex.zl; this file holds what Zell cannot
 * express cheaply: the Unicode category/script tables and simple case
 * folding (generated from Go's unicode package, std_regex_tables.h),
 * zeroed []int allocation, a non-copying substring search for the literal
 * prefilter and a try-lock for the shared DFA cache. */
#include "rt_internal.h"

#include <string.h>

#include "std_regex_tables.h"

static inline int64_t slen(const zell_string *s) { return s ? s->len : 0; }
static inline const char *sdata(const zell_string *s) { return s ? s->data : ""; }

/* Flat [lo, hi, lo, hi, ...] ranges of the Unicode class `name` (a general
 * category like "L" / "Lu" or a script like "Greek", or "Any"); an empty
 * slice if the name is unknown. */
void zell_regex_rt_unicode(const zell_string *name, zell_slice *out) {
    const char *n = zell_string_cstr(name);
    if (strcmp(n, "Any") == 0) {
        zell_slice_new(out, 8, 2, 2);
        int64_t *d = out->data;
        d[0] = 0;
        d[1] = 0x10FFFF;
        return;
    }
    size_t lo = 0, hi = sizeof zell__regex_uclasses / sizeof zell__regex_uclasses[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(zell__regex_uclasses[mid].name, n);
        if (c == 0) {
            int32_t off = zell__regex_uclasses[mid].off, cnt = zell__regex_uclasses[mid].cnt;
            zell_slice_new(out, 8, cnt * 2, cnt * 2);
            int64_t *d = out->data;
            for (int32_t i = 0; i < cnt * 2; i++) d[i] = zell__regex_uranges[off * 2 + i];
            return;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    memset(out, 0, sizeof *out);
}

/* The next rune of c's simple case-folding orbit (Go unicode.SimpleFold),
 * or c itself if it has none. */
int64_t zell_regex_rt_fold(int64_t c) {
    size_t lo = 0, hi = sizeof zell__regex_fold / sizeof zell__regex_fold[0] / 2;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int64_t k = zell__regex_fold[mid * 2];
        if (k == c) return zell__regex_fold[mid * 2 + 1];
        if (k < c) lo = mid + 1; else hi = mid;
    }
    return c;
}

/* A zeroed []int of length n. */
void zell_regex_rt_ints(int64_t n, zell_slice *out) {
    if (n <= 0) { memset(out, 0, sizeof *out); return; }
    zell_slice_new(out, 8, n, n);
}

/* memmem (a GNU/BSD extension, absent from some C libraries). */
static const char *find_bytes(const char *h, int64_t hn, const char *nd, int64_t m) {
    while (hn >= m) {
        const char *c = memchr(h, nd[0], (size_t)(hn - m + 1));
        if (!c) return NULL;
        if (memcmp(c, nd, (size_t)m) == 0) return c;
        hn -= c + 1 - h;
        h = c + 1;
    }
    return NULL;
}

/* Byte offset of the first occurrence of sub in s at or after from, or -1. */
int64_t zell_regex_rt_index(const zell_string *s, const zell_string *sub, int64_t from) {
    int64_t n = slen(s), m = slen(sub);
    if (from < 0) from = 0;
    if (from > n) return -1;
    if (m == 0) return from;
    const char *p = sdata(s), *q = sdata(sub);
    if (m == 1) {
        const char *r = memchr(p + from, q[0], (size_t)(n - from));
        return r ? r - p : -1;
    }
    const char *r = find_bytes(p + from, n - from, q, m);
    return r ? r - p : -1;
}

/* Try-lock / unlock of an int slot (0 free, 1 held). */
bool zell_regex_rt_try_lock(int64_t *slot) {
    if (!slot) zell_panic_nil();
    return __atomic_load_n(slot, __ATOMIC_RELAXED) == 0 && __atomic_exchange_n(slot, 1, __ATOMIC_ACQUIRE) == 0;
}

void zell_regex_rt_unlock(int64_t *slot) {
    if (!slot) zell_panic_nil();
    __atomic_store_n(slot, 0, __ATOMIC_RELEASE);
}

/* A zeroed []byte of length n. */
void zell_regex_rt_bytes(int64_t n, zell_slice *out) {
    if (n <= 0) { memset(out, 0, sizeof *out); return; }
    zell_slice_new(out, 1, n, n);
}
