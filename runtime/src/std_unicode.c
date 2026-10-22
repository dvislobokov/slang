/* std_unicode.c - unicode character classes and case mapping over
 * the generated range tables in std_unicode_tables.c (Unicode 15, from
 * Go's unicode package; regenerate with scripts/gen-unicode-tables.py).
 * Semantics follow Go's unicode package exactly. */
#include <stdbool.h>
#include <stdint.h>

#define TABLE(n)                                  \
    extern const uint32_t zell__unicode_##n[][3]; \
    extern const int zell__unicode_##n##_len;
TABLE(c)
TABLE(cc)
TABLE(l)
TABLE(lu)
TABLE(ll)
TABLE(lt)
TABLE(m)
TABLE(n)
TABLE(nd)
TABLE(p)
TABLE(s)
TABLE(zs)
TABLE(white_space)
extern const int32_t zell__unicode_case[][5];
extern const int zell__unicode_case_len;
extern const char zell__unicode_version[];

bool zell_unicode_is_letter(int32_t r);
bool zell_unicode_is_digit(int32_t r);
bool zell_unicode_is_number(int32_t r);
bool zell_unicode_is_space(int32_t r);
bool zell_unicode_is_upper(int32_t r);
bool zell_unicode_is_lower(int32_t r);
bool zell_unicode_is_title(int32_t r);
bool zell_unicode_is_punct(int32_t r);
bool zell_unicode_is_symbol(int32_t r);
bool zell_unicode_is_mark(int32_t r);
bool zell_unicode_is_control(int32_t r);
bool zell_unicode_is_print(int32_t r);
bool zell_unicode_is_graphic(int32_t r);
int32_t zell_unicode_to_upper(int32_t r);
int32_t zell_unicode_to_lower(int32_t r);
int32_t zell_unicode_to_title(int32_t r);
int32_t zell_unicode_simple_fold(int32_t r);

static bool in_table(const uint32_t (*t)[3], int n, int32_t r) {
    if (r < 0) return false;
    uint32_t u = (uint32_t)r;
    int lo = 0, hi = n;
    while (lo < hi) {
        int m = lo + (hi - lo) / 2;
        if (u < t[m][0]) {
            hi = m;
        } else if (u > t[m][1]) {
            lo = m + 1;
        } else {
            return t[m][2] == 1 || (u - t[m][0]) % t[m][2] == 0;
        }
    }
    return false;
}

#define IS(n, r) in_table(zell__unicode_##n, zell__unicode_##n##_len, r)

static bool ascii_letter(int32_t r) { return (r | 0x20) >= 'a' && (r | 0x20) <= 'z'; }

bool zell_unicode_is_letter(int32_t r) {
    if (r < 0x80) return ascii_letter(r);
    return IS(l, r);
}
bool zell_unicode_is_digit(int32_t r) {
    if (r < 0x80) return r >= '0' && r <= '9';
    return IS(nd, r);
}
bool zell_unicode_is_number(int32_t r) {
    if (r < 0x80) return r >= '0' && r <= '9';
    return IS(n, r);
}
bool zell_unicode_is_space(int32_t r) {
    if (r <= 0xFF) {
        switch (r) {
        case '\t': case '\n': case '\v': case '\f': case '\r': case ' ': case 0x85: case 0xA0:
            return true;
        }
        return false;
    }
    return IS(white_space, r);
}
bool zell_unicode_is_upper(int32_t r) {
    if (r < 0x80) return r >= 'A' && r <= 'Z';
    return IS(lu, r);
}
bool zell_unicode_is_lower(int32_t r) {
    if (r < 0x80) return r >= 'a' && r <= 'z';
    return IS(ll, r);
}
bool zell_unicode_is_title(int32_t r) {
    if (r < 0x80) return false;
    return IS(lt, r);
}
bool zell_unicode_is_punct(int32_t r) { return IS(p, r); }
bool zell_unicode_is_symbol(int32_t r) { return IS(s, r); }
bool zell_unicode_is_mark(int32_t r) { return IS(m, r); }
bool zell_unicode_is_control(int32_t r) {
    /* Go: only the Latin-1 control characters (C0, DEL, C1). */
    return r >= 0 && r <= 0xFF && IS(cc, r);
}
bool zell_unicode_is_graphic(int32_t r) {
    return IS(l, r) || IS(m, r) || IS(n, r) || IS(p, r) || IS(s, r) || IS(zs, r);
}
bool zell_unicode_is_print(int32_t r) {
    if (r == ' ') return true;
    return IS(l, r) || IS(m, r) || IS(n, r) || IS(p, r) || IS(s, r);
}

/* Go's unicode.to: which = 0 upper, 1 lower, 2 title. */
static int32_t to_case(int which, int32_t r) {
    int lo = 0, hi = zell__unicode_case_len;
    while (lo < hi) {
        int m = lo + (hi - lo) / 2;
        const int32_t *cr = zell__unicode_case[m];
        if (cr[0] <= r && r <= cr[1]) {
            int32_t delta = cr[2 + which];
            if (delta > 0x10FFFF) return cr[0] + (((r - cr[0]) & ~1) | (which & 1));
            return r + delta;
        }
        if (r < cr[0]) {
            hi = m;
        } else {
            lo = m + 1;
        }
    }
    return r;
}

int32_t zell_unicode_to_upper(int32_t r) {
    if (r < 0x80) return (r >= 'a' && r <= 'z') ? r - 32 : r;
    return to_case(0, r);
}
int32_t zell_unicode_to_lower(int32_t r) {
    if (r < 0x80) return (r >= 'A' && r <= 'Z') ? r + 32 : r;
    return to_case(1, r);
}
int32_t zell_unicode_to_title(int32_t r) {
    if (r < 0x80) return (r >= 'a' && r <= 'z') ? r - 32 : r;
    return to_case(2, r);
}

/* The next rune in r's simple case-folding orbit (Go's SimpleFold, without
 * the asciiFold/caseOrbit special tables: the orbit is approximated by
 * upper/lower mapping, exact for all two-element orbits). */
int32_t zell_unicode_simple_fold(int32_t r) {
    if (r < 0 || r > 0x10FFFF) return r;
    int32_t l = zell_unicode_to_lower(r);
    if (l != r) return l;
    return zell_unicode_to_upper(r);
}
