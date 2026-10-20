/* std_fmt.c - std.fmt.Format (slang_fmt_format). The compiler converts every
 * argument to a string (as println would print it) and passes them as a
 * []string; this function only substitutes the placeholders. */
#include "rt_internal.h"

#include <string.h>

/* If tmpl[i] starts a placeholder "{N}" (N = one or more ASCII digits) whose
 * index is < nargs, store the index in *idx and the placeholder length in *plen.
 * Indices too large to be valid are rejected without overflowing. */
static bool placeholder(const char *t, int64_t n, int64_t i, int64_t nargs,
                        int64_t *idx, int64_t *plen) {
    if (t[i] != '{') return false;
    int64_t j = i + 1, v = 0;
    bool big = false;
    while (j < n && t[j] >= '0' && t[j] <= '9') {
        if (v > (INT64_MAX - 9) / 10) big = true;
        else v = v * 10 + (t[j] - '0');
        j++;
    }
    if (j == i + 1 || j >= n || t[j] != '}' || big || v >= nargs) return false;
    *idx = v;
    *plen = j + 1 - i;
    return true;
}

slang_string *slang_fmt_format(const slang_string *tmpl, const slang_slice *args) {
    const char *t = slang_string_data(tmpl);
    int64_t n = slang_string_len(tmpl);
    int64_t nargs = args ? args->len : 0;
    slang_string *const *av = nargs ? args->data : NULL;
    int64_t total = 0, idx, pl;
    for (int64_t i = 0; i < n;) {
        if (placeholder(t, n, i, nargs, &idx, &pl)) { total += slang_string_len(av[idx]); i += pl; }
        else { total++; i++; }
    }
    slang_string *r = slang__string_alloc(total);
    char *w = r->data;
    for (int64_t i = 0; i < n;) {
        if (placeholder(t, n, i, nargs, &idx, &pl)) {
            int64_t l = slang_string_len(av[idx]);
            if (l) memcpy(w, slang_string_data(av[idx]), (size_t)l);
            w += l;
            i += pl;
        } else {
            *w++ = t[i++];
        }
    }
    return r;
}
