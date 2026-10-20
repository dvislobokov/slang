/* std_regex.c - the C half of std.regex (runtime/ABI.md 14.5). The engine
 * (parser, compiler, Pike VM, lazy DFA) is Slang code in
 * compiler/src/sema/stdlib_src/regex.sl; this file holds what Slang cannot
 * express cheaply: the Unicode category/script tables and simple case
 * folding (generated from Go's unicode package, std_regex_tables.h),
 * zeroed []int allocation, a non-copying substring search for the literal
 * prefilter and a try-lock for the shared DFA cache. */
#include "rt_internal.h"

#include <string.h>

#include "std_regex_tables.h"

static inline int64_t slen(const slang_string *s) { return s ? s->len : 0; }
static inline const char *sdata(const slang_string *s) { return s ? s->data : ""; }

/* Flat [lo, hi, lo, hi, ...] ranges of the Unicode class `name` (a general
 * category like "L" / "Lu" or a script like "Greek", or "Any"); an empty
 * slice if the name is unknown. */
void slang_regex_rt_unicode(const slang_string *name, slang_slice *out) {
    const char *n = sdata(name);
    if (strcmp(n, "Any") == 0) {
        slang_slice_new(out, 8, 2, 2);
        int64_t *d = out->data;
        d[0] = 0;
        d[1] = 0x10FFFF;
        return;
    }
    size_t lo = 0, hi = sizeof slang__regex_uclasses / sizeof slang__regex_uclasses[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(slang__regex_uclasses[mid].name, n);
        if (c == 0) {
            int32_t off = slang__regex_uclasses[mid].off, cnt = slang__regex_uclasses[mid].cnt;
            slang_slice_new(out, 8, cnt * 2, cnt * 2);
            int64_t *d = out->data;
            for (int32_t i = 0; i < cnt * 2; i++) d[i] = slang__regex_uranges[off * 2 + i];
            return;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    memset(out, 0, sizeof *out);
}

/* The next rune of c's simple case-folding orbit (Go unicode.SimpleFold),
 * or c itself if it has none. */
int64_t slang_regex_rt_fold(int64_t c) {
    size_t lo = 0, hi = sizeof slang__regex_fold / sizeof slang__regex_fold[0] / 2;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int64_t k = slang__regex_fold[mid * 2];
        if (k == c) return slang__regex_fold[mid * 2 + 1];
        if (k < c) lo = mid + 1; else hi = mid;
    }
    return c;
}

/* A zeroed []int of length n. */
void slang_regex_rt_ints(int64_t n, slang_slice *out) {
    if (n <= 0) { memset(out, 0, sizeof *out); return; }
    slang_slice_new(out, 8, n, n);
}

/* Byte offset of the first occurrence of sub in s at or after from, or -1. */
int64_t slang_regex_rt_index(const slang_string *s, const slang_string *sub, int64_t from) {
    int64_t n = slen(s), m = slen(sub);
    if (from < 0) from = 0;
    if (from > n) return -1;
    if (m == 0) return from;
    const char *p = sdata(s), *q = sdata(sub);
    if (m == 1) {
        const char *r = memchr(p + from, q[0], (size_t)(n - from));
        return r ? r - p : -1;
    }
    const char *r = memmem(p + from, (size_t)(n - from), q, (size_t)m);
    return r ? r - p : -1;
}

/* Try-lock / unlock of an int slot (0 free, 1 held). */
bool slang_regex_rt_try_lock(int64_t *slot) {
    if (!slot) slang_panic_nil();
    return __atomic_load_n(slot, __ATOMIC_RELAXED) == 0 && __atomic_exchange_n(slot, 1, __ATOMIC_ACQUIRE) == 0;
}

void slang_regex_rt_unlock(int64_t *slot) {
    if (!slot) slang_panic_nil();
    __atomic_store_n(slot, 0, __ATOMIC_RELEASE);
}

/* A zeroed []byte of length n. */
void slang_regex_rt_bytes(int64_t n, slang_slice *out) {
    if (n <= 0) { memset(out, 0, sizeof *out); return; }
    slang_slice_new(out, 1, n, n);
}
