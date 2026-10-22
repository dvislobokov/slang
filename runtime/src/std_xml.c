/* std_xml.c - the C half of encoding.xml (runtime/ABI.md 19.4): a
 * tokenizer. The pull Reader, namespaces, tag matching, the Writer and the
 * typed (de)coders are Zell (compiler/src/sema/stdlib_src/xml.zl).
 *
 * zell_encoding_xml_rt_scan turns a document into a []string of tokens
 * tagged by their first byte:
 *   "S<line> <qname>"   start tag; followed by attribute pairs
 *   "A<qname>" "V<value>" (value with entities decoded), then
 *   ">" (open) or "/" (self-closing: an end follows implicitly)
 *   "E<line> <qname>"   end tag
 *   "T<line> <text>"    character data (entities decoded, CDATA merged)
 *   "C<line> <text>"    comment
 *   "P<line> <target>" "I<inst>" processing instruction (<?xml ...?> too)
 *   "D<line> <text>"    a <!DOCTYPE ...> or other directive, verbatim:
 *                       never interpreted (no DTD, no entity expansion)
 * Only the predefined entities and character references are decoded;
 * with strict, any other entity is an error, otherwise it is kept as is.
 * Errors: "xml: <what> on line N". */
#include "rt_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    int64_t n, i, line;
    bool strict;
    zell_slice *out;
    const char *err;
    int64_t err_line;
    char *buf;
    int64_t blen, bcap;
} xs;

static void push(xs *s, char kind, const char *p, int64_t n) {
    zell_string *t = zell__string_alloc(n + 1);
    t->data[0] = kind;
    if (n) memcpy(t->data + 1, p, (size_t)n);
    t->data[n + 1] = 0;
    zell_slice_append(s->out, 8, &zell_ops_ref, &t);
    zell_release(t);
}

static void push_line(xs *s, char kind, int64_t line, const char *p, int64_t n) {
    char num[32];
    int k = snprintf(num, sizeof num, "%lld ", (long long)line);
    zell_string *t = zell__string_alloc(1 + k + n);
    t->data[0] = kind;
    memcpy(t->data + 1, num, (size_t)k);
    if (n) memcpy(t->data + 1 + k, p, (size_t)n);
    t->data[1 + k + n] = 0;
    zell_slice_append(s->out, 8, &zell_ops_ref, &t);
    zell_release(t);
}

static int fail(xs *s, const char *what) {
    if (!s->err) {
        s->err = what;
        s->err_line = s->line;
    }
    return 0;
}

static void bput(xs *s, const char *p, int64_t n) {
    if (s->blen + n > s->bcap) {
        int64_t cap = s->bcap ? s->bcap * 2 : 256;
        while (cap < s->blen + n) cap *= 2;
        s->buf = zell_realloc(s->buf, (size_t)cap);
        s->bcap = cap;
    }
    memcpy(s->buf + s->blen, p, (size_t)n);
    s->blen += n;
}

static void bput_rune(xs *s, uint32_t c) {
    char w[4];
    int n;
    if (c < 0x80) { w[0] = (char)c; n = 1; }
    else if (c < 0x800) { w[0] = (char)(0xC0 | c >> 6); w[1] = (char)(0x80 | (c & 63)); n = 2; }
    else if (c < 0x10000) { w[0] = (char)(0xE0 | c >> 12); w[1] = (char)(0x80 | ((c >> 6) & 63)); w[2] = (char)(0x80 | (c & 63)); n = 3; }
    else { w[0] = (char)(0xF0 | c >> 18); w[1] = (char)(0x80 | ((c >> 12) & 63)); w[2] = (char)(0x80 | ((c >> 6) & 63)); w[3] = (char)(0x80 | (c & 63)); n = 4; }
    bput(s, w, n);
}

static inline int name_start(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80; }
static inline int name_char(uint8_t c) { return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }
static inline int is_ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void skip_ws(xs *s) {
    while (s->i < s->n && is_ws(s->p[s->i])) {
        if (s->p[s->i] == '\n') s->line++;
        s->i++;
    }
}

/* Decodes an entity at p[i] == '&' into the buffer; returns 0 on error. */
static int entity(xs *s) {
    int64_t j = s->i + 1;
    while (j < s->n && j - s->i < 40 && s->p[j] != ';' && !is_ws(s->p[j]) && s->p[j] != '<' && s->p[j] != '&') j++;
    if (j >= s->n || s->p[j] != ';') {
        if (s->strict) return fail(s, "invalid character entity (no semicolon)");
        bput(s, "&", 1);
        s->i++;
        return 1;
    }
    const char *e = (const char *)s->p + s->i + 1;
    int64_t len = j - s->i - 1;
    if (len >= 2 && e[0] == '#') {
        uint32_t v = 0;
        int ok = 1;
        if (e[1] == 'x') {
            if (len < 3) ok = 0;
            for (int64_t k = 2; k < len && ok; k++) {
                char c = e[k];
                int h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (h < 0 || v > 0x10FFFF) ok = 0;
                else v = v * 16 + (uint32_t)h;
            }
        } else {
            for (int64_t k = 1; k < len && ok; k++) {
                char c = e[k];
                if (c < '0' || c > '9' || v > 0x10FFFF) ok = 0;
                else v = v * 10 + (uint32_t)(c - '0');
            }
        }
        if (!ok || v == 0 || v > 0x10FFFF || (v >= 0xD800 && v < 0xE000)) return fail(s, "invalid character reference");
        bput_rune(s, v);
    } else if (len == 2 && memcmp(e, "lt", 2) == 0) {
        bput(s, "<", 1);
    } else if (len == 2 && memcmp(e, "gt", 2) == 0) {
        bput(s, ">", 1);
    } else if (len == 3 && memcmp(e, "amp", 3) == 0) {
        bput(s, "&", 1);
    } else if (len == 4 && memcmp(e, "apos", 4) == 0) {
        bput(s, "'", 1);
    } else if (len == 4 && memcmp(e, "quot", 4) == 0) {
        bput(s, "\"", 1);
    } else {
        if (s->strict) return fail(s, "undefined entity (DTD entities are not expanded)");
        bput(s, (const char *)s->p + s->i, j - s->i + 1);
    }
    s->i = j + 1;
    return 1;
}

static int name(xs *s, int64_t *start, int64_t *len) {
    if (s->i >= s->n || !name_start(s->p[s->i])) return fail(s, "expected a name");
    *start = s->i;
    while (s->i < s->n && name_char(s->p[s->i])) s->i++;
    *len = s->i - *start;
    return 1;
}

static int start_tag(xs *s) {
    int64_t line = s->line, ns, nl;
    s->i++;
    if (!name(s, &ns, &nl)) return 0;
    push_line(s, 'S', line, (const char *)s->p + ns, nl);
    for (;;) {
        int64_t before = s->i;
        skip_ws(s);
        if (s->i >= s->n) return fail(s, "unexpected end of input in a start tag");
        uint8_t c = s->p[s->i];
        if (c == '>') {
            s->i++;
            push(s, '>', "", 0);
            return 1;
        }
        if (c == '/') {
            if (s->i + 1 >= s->n || s->p[s->i + 1] != '>') return fail(s, "expected /> to end an empty element");
            s->i += 2;
            push(s, '/', "", 0);
            return 1;
        }
        if (s->i == before) return fail(s, "expected white space before an attribute");
        int64_t as, al;
        if (!name(s, &as, &al)) return fail(s, "expected an attribute name");
        skip_ws(s);
        if (s->i >= s->n || s->p[s->i] != '=') return fail(s, "attribute without a value");
        s->i++;
        skip_ws(s);
        if (s->i >= s->n || (s->p[s->i] != '"' && s->p[s->i] != '\'')) return fail(s, "unquoted attribute value");
        uint8_t q = s->p[s->i++];
        s->blen = 0;
        int64_t run = s->i;
        for (;;) {
            if (s->i >= s->n) return fail(s, "unexpected end of input in an attribute value");
            uint8_t d = s->p[s->i];
            if (d == q) break;
            if (d == '<') return fail(s, "< in an attribute value");
            if (d == '&') {
                bput(s, (const char *)s->p + run, s->i - run);
                if (!entity(s)) return 0;
                run = s->i;
                continue;
            }
            if (d == '\n') s->line++;
            if (d == '\t' || d == '\n' || d == '\r') {
                /* attribute value normalization: white space becomes a space */
                bput(s, (const char *)s->p + run, s->i - run);
                if (!(d == '\n' && s->i > 0 && s->p[s->i - 1] == '\r')) bput(s, " ", 1);
                s->i++;
                run = s->i;
                continue;
            }
            s->i++;
        }
        bput(s, (const char *)s->p + run, s->i - run);
        s->i++;
        push(s, 'A', (const char *)s->p + as, al);
        push(s, 'V', s->buf, s->blen);
    }
}

static int find(xs *s, const char *lit) {
    size_t k = strlen(lit);
    for (int64_t j = s->i; j + (int64_t)k <= s->n; j++) {
        if (memcmp(s->p + j, lit, k) == 0) {
            s->i = j;
            return 1;
        }
        if (s->p[j] == '\n') s->line++;
    }
    return 0;
}

static void scan(xs *s) {
    int64_t text_start = s->i, text_line = s->line;
    s->blen = 0;
#define FLUSH_TEXT() do { \
        bput(s, (const char *)s->p + text_start, s->i - text_start); \
        if (s->blen) push_line(s, 'T', text_line, s->buf, s->blen); \
        s->blen = 0; \
    } while (0)
    while (s->i < s->n) {
        uint8_t c = s->p[s->i];
        if (c == '\n') {
            s->line++;
            s->i++;
            continue;
        }
        if (c == '\r') {
            /* line ends are normalized to \n */
            bput(s, (const char *)s->p + text_start, s->i - text_start);
            if (s->i + 1 >= s->n || s->p[s->i + 1] != '\n') bput(s, "\n", 1);
            s->i++;
            text_start = s->i;
            continue;
        }
        if (c == '&') {
            bput(s, (const char *)s->p + text_start, s->i - text_start);
            if (!entity(s)) return;
            text_start = s->i;
            continue;
        }
        if (c != '<') {
            s->i++;
            continue;
        }
        if (s->i + 9 <= s->n && memcmp(s->p + s->i, "<![CDATA[", 9) == 0) {
            bput(s, (const char *)s->p + text_start, s->i - text_start);
            s->i += 9;
            int64_t st = s->i;
            if (!find(s, "]]>")) {
                fail(s, "unterminated CDATA section");
                return;
            }
            bput(s, (const char *)s->p + st, s->i - st);
            s->i += 3;
            text_start = s->i;
            continue;
        }
        FLUSH_TEXT();
        int64_t line0 = s->line;
        if (s->i + 4 <= s->n && memcmp(s->p + s->i, "<!--", 4) == 0) {
            s->i += 4;
            int64_t st = s->i;
            if (!find(s, "-->")) {
                fail(s, "unterminated comment");
                return;
            }
            push_line(s, 'C', line0, (const char *)s->p + st, s->i - st);
            s->i += 3;
        } else if (s->i + 2 <= s->n && s->p[s->i + 1] == '?') {
            s->i += 2;
            int64_t ts, tl;
            if (!name(s, &ts, &tl)) return;
            int64_t st = s->i;
            if (!find(s, "?>")) {
                fail(s, "unterminated processing instruction");
                return;
            }
            int64_t is = st;
            while (is < s->i && is_ws(s->p[is])) is++;
            push_line(s, 'P', line0, (const char *)s->p + ts, tl);
            push(s, 'I', (const char *)s->p + is, s->i - is);
            s->i += 2;
        } else if (s->i + 2 <= s->n && s->p[s->i + 1] == '!') {
            /* <!DOCTYPE ...> with an optional [internal subset], kept verbatim */
            int64_t st = s->i + 2;
            int depth = 0;
            int64_t j = st;
            uint8_t quote = 0;
            for (; j < s->n; j++) {
                uint8_t d = s->p[j];
                if (d == '\n') s->line++;
                if (quote) {
                    if (d == quote) quote = 0;
                } else if (d == '"' || d == '\'') {
                    quote = d;
                } else if (d == '[') {
                    depth++;
                } else if (d == ']') {
                    depth--;
                } else if (d == '>' && depth <= 0) {
                    break;
                }
            }
            if (j >= s->n) {
                fail(s, "unterminated directive");
                return;
            }
            push_line(s, 'D', line0, (const char *)s->p + st, j - st);
            s->i = j + 1;
        } else if (s->i + 1 < s->n && s->p[s->i + 1] == '/') {
            int64_t line = s->line, ns, nl;
            s->i += 2;
            if (!name(s, &ns, &nl)) return;
            skip_ws(s);
            if (s->i >= s->n || s->p[s->i] != '>') {
                fail(s, "expected > to end an end tag");
                return;
            }
            s->i++;
            push_line(s, 'E', line, (const char *)s->p + ns, nl);
        } else {
            if (!start_tag(s)) return;
        }
        s->blen = 0;
        text_start = s->i;
        text_line = s->line;
    }
    FLUSH_TEXT();
#undef FLUSH_TEXT
}

void zell_encoding_xml_rt_scan(const zell_string *text, bool strict, zell_slice *out, zell_error **err) {
    xs s;
    memset(&s, 0, sizeof s);
    s.p = (const uint8_t *)zell_string_data(text);
    s.n = zell_string_len(text);
    s.line = 1;
    s.strict = strict;
    s.out = out;
    zell_slice_new_ops(out, 8, 0, 16, &zell_ops_ref);
    /* a UTF-8 byte order mark is skipped */
    if (s.n >= 3 && s.p[0] == 0xEF && s.p[1] == 0xBB && s.p[2] == 0xBF) s.i = 3;
    scan(&s);
    zell_free(s.buf);
    if (err) *err = NULL;
    if (s.err) {
        char msg[200];
        snprintf(msg, sizeof msg, "xml: %s on line %lld", s.err, (long long)s.err_line);
        zell_slice_release(out);
        if (err) *err = zell_error_new_cstr(msg);
    }
}

/* Escapes text for character data (attr: also quotes and white space). */
zell_string *zell_encoding_xml_rt_escape(const zell_string *text, bool attr) {
    const char *p = zell_string_data(text);
    int64_t n = zell_string_len(text), extra = 0;
    for (int64_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '&') extra += 4;
        else if (c == '<' || c == '>') extra += 3;
        else if (attr && c == '"') extra += 5;
        else if (attr && (c == '\n' || c == '\t' || c == '\r')) extra += 4;
        else if (!attr && c == '\r') extra += 4;
    }
    if (extra == 0) {
        zell_retain((void *)text);
        return text ? (zell_string *)text : zell__string_alloc(0);
    }
    zell_string *r = zell__string_alloc(n + extra);
    char *w = r->data;
    for (int64_t i = 0; i < n; i++) {
        char c = p[i];
        const char *e = NULL;
        if (c == '&') e = "&amp;";
        else if (c == '<') e = "&lt;";
        else if (c == '>') e = "&gt;";
        else if (attr && c == '"') e = "&quot;";
        else if (attr && c == '\n') e = "&#xA;";
        else if (attr && c == '\t') e = "&#x9;";
        else if (c == '\r') e = "&#xD;";
        if (e) {
            size_t k = strlen(e);
            memcpy(w, e, k);
            w += k;
        } else {
            *w++ = c;
        }
    }
    *w = 0;
    return r;
}
