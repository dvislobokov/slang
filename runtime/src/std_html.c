/* std_html.c - html (runtime/ABI.md 19.5): HTML escaping and
 * unescaping with the full WHATWG named character reference table
 * (std_html_entities.h). Semantics follow Go's html package: Escape
 * replaces < > & ' " ; Unescape decodes named references (also the legacy
 * ones written without ';', longest match), &#NNN; and &#xHH; (invalid or
 * out-of-range code points become U+FFFD, Windows-1252 C1 remapping as
 * in the HTML5 spec). */
#include "rt_internal.h"

#include <string.h>

#include "std_html_entities.h"

static inline int64_t slen(const zell_string *s) { return s ? s->len : 0; }
static inline const char *sdata(const zell_string *s) { return s ? s->data : ""; }

zell_string *zell_html_escape(const zell_string *s) {
    const char *p = sdata(s);
    int64_t n = slen(s), extra = 0;
    for (int64_t i = 0; i < n; i++) {
        switch (p[i]) {
        case '<': case '>': extra += 3; break;
        case '&': case '\'': case '"': extra += 4; break;
        default: break;
        }
    }
    if (extra == 0) {
        if (!s) return zell__string_alloc(0);
        zell_retain((void *)s);
        return (zell_string *)s;
    }
    zell_string *r = zell__string_alloc(n + extra);
    char *w = r->data;
    for (int64_t i = 0; i < n; i++) {
        const char *e = NULL;
        switch (p[i]) {
        case '<': e = "&lt;"; break;
        case '>': e = "&gt;"; break;
        case '&': e = "&amp;"; break;
        case '\'': e = "&#39;"; break;
        case '"': e = "&#34;"; break;
        default: *w++ = p[i]; continue;
        }
        size_t k = strlen(e);
        memcpy(w, e, k);
        w += k;
    }
    *w = 0;
    return r;
}

static int put_utf8(char *w, uint32_t c) {
    if (c < 0x80) { w[0] = (char)c; return 1; }
    if (c < 0x800) { w[0] = (char)(0xC0 | c >> 6); w[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { w[0] = (char)(0xE0 | c >> 12); w[1] = (char)(0x80 | ((c >> 6) & 63)); w[2] = (char)(0x80 | (c & 63)); return 3; }
    w[0] = (char)(0xF0 | c >> 18); w[1] = (char)(0x80 | ((c >> 12) & 63)); w[2] = (char)(0x80 | ((c >> 6) & 63)); w[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* HTML5 replacements for numeric references 0x80..0x9F (Windows-1252). */
static const uint16_t c1[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

/* The entity whose name is the longest prefix of p[0..n) ending in ';' or
 * a legacy name; its index or -1, length in *len. */
static int find_entity(const char *p, int64_t n, int64_t *len) {
    int best = -1;
    int64_t best_len = 0;
    size_t lo = 0, hi = sizeof zell__html_entities / sizeof zell__html_entities[0];
    /* names are at most 32 bytes: try each prefix length by binary search */
    for (int64_t k = 1; k <= n && k <= 32; k++) {
        size_t a = lo, b = hi;
        while (a < b) {
            size_t m = (a + b) / 2;
            int c = strncmp(zell__html_entities[m].name, p, (size_t)k);
            if (c == 0 && strlen(zell__html_entities[m].name) > (size_t)k) c = 1;
            if (c < 0) a = m + 1; else b = m;
        }
        if (a < hi && strlen(zell__html_entities[a].name) == (size_t)k && strncmp(zell__html_entities[a].name, p, (size_t)k) == 0) {
            best = (int)a;
            best_len = k;
        }
    }
    *len = best_len;
    return best;
}

zell_string *zell_html_unescape(const zell_string *s) {
    const char *p = sdata(s);
    int64_t n = slen(s);
    if (!memchr(p, '&', (size_t)n)) {
        if (!s) return zell__string_alloc(0);
        zell_retain((void *)s);
        return (zell_string *)s;
    }
    /* the result is never longer than 2x... entities expand to at most 4 bytes from >= 2 */
    char *buf = zell_alloc(n * 2 + 8);
    int64_t w = 0;
    for (int64_t i = 0; i < n;) {
        if (p[i] != '&') {
            buf[w++] = p[i++];
            continue;
        }
        if (i + 1 < n && p[i + 1] == '#') {
            int64_t j = i + 2;
            int hex = 0;
            if (j < n && (p[j] == 'x' || p[j] == 'X')) {
                hex = 1;
                j++;
            }
            int64_t start = j;
            uint32_t v = 0;
            int big = 0;
            while (j < n) {
                char c = p[j];
                int d = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) break;
                if (v > 0x10FFFF) big = 1;
                else v = v * (hex ? 16 : 10) + (uint32_t)d;
                j++;
            }
            if (j == start && !(hex && j < n && p[j] == ';')) {
                /* no digits (as Go: "&#x;" still counts as a reference) */
                buf[w++] = p[i++];
                continue;
            }
            if (j < n && p[j] == ';') j++;
            if (big || v > 0x10FFFF || v == 0 || (v >= 0xD800 && v < 0xE000)) v = 0xFFFD;
            else if (v >= 0x80 && v <= 0x9F) v = c1[v - 0x80];
            w += put_utf8(buf + w, v);
            i = j;
            continue;
        }
        int64_t len;
        int e = find_entity(p + i + 1, n - i - 1, &len);
        if (e < 0) {
            buf[w++] = p[i++];
            continue;
        }
        const char *val = zell__html_entities[e].value;
        size_t vl = strlen(val);
        memcpy(buf + w, val, vl);
        w += (int64_t)vl;
        i += 1 + len;
    }
    zell_string *r = zell_string_new(buf, w);
    zell_free(buf);
    return r;
}
