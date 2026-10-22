/* std_json.c - the C half of encoding.json (runtime/ABI.md 17.4):
 * a tokenizer and string quoting. The parser and the typed (de)coders are
 * written in Zell (compiler/src/sema/reflect_src/json.zl) on top of these.
 *
 * zell_encoding_json_scan turns a JSON text (RFC 8259) into a []string of
 * tokens, one per lexical element, tagged by their first byte:
 *   "{" "}" "[" "]" ":" ","   punctuation
 *   "s<bytes>"                a string, unescaped (UTF-8)
 *   "n<text>"                 a number, as written (valid JSON syntax)
 *   "t" "f" "z"               true, false, null
 * Grammar (nesting, commas) is checked by the Zell parser; lexical errors
 * fail here with "json: <what> at offset N". */
#include "rt_internal.h"

#include <stdio.h>
#include <string.h>

static void json_fail(zell_slice *res, zell_error **err, const char *what, int64_t off) {
    char buf[160];
    snprintf(buf, sizeof buf, "json: %s at offset %lld", what, (long long)off);
    zell_slice_release(res);
    if (err) *err = zell_error_new_cstr(buf);
}

/* One-byte tokens (punctuation, true/false/null) are immortal statics. */
#define TOK1(name, lit) static ZELL_STRING_STORAGE(2) name = ZELL_STRING_INIT(name, lit)
TOK1(tok_lbrace, "{"); TOK1(tok_rbrace, "}"); TOK1(tok_lbrack, "["); TOK1(tok_rbrack, "]");
TOK1(tok_colon, ":"); TOK1(tok_comma, ","); TOK1(tok_t, "t"); TOK1(tok_f, "f"); TOK1(tok_z, "z");

static zell_string *token1(char c) {
    switch (c) {
    case '{': return (zell_string *)&tok_lbrace;
    case '}': return (zell_string *)&tok_rbrace;
    case '[': return (zell_string *)&tok_lbrack;
    case ']': return (zell_string *)&tok_rbrack;
    case ':': return (zell_string *)&tok_colon;
    case ',': return (zell_string *)&tok_comma;
    case 't': return (zell_string *)&tok_t;
    case 'f': return (zell_string *)&tok_f;
    case 'z': return (zell_string *)&tok_z;
    default: return NULL;
    }
}

static void push_token(zell_slice *res, const char *p, int64_t n) {
    zell_string *t = n == 1 ? token1(p[0]) : NULL;
    if (t) {
        zell_slice_append(res, 8, &zell_ops_ref, &t);
        return;
    }
    zell_string *s = zell_string_new(p, n);
    zell_slice_append(res, 8, &zell_ops_ref, &s);
    zell_release(s);
}

static int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(const uint8_t *p, int64_t n, int64_t i, uint32_t *out) {
    if (i + 4 > n) return 0;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexval(p[i + k]);
        if (h < 0) return 0;
        v = v << 4 | (uint32_t)h;
    }
    *out = v;
    return 1;
}

static int64_t put_utf8(char *w, uint32_t c) {
    if (c < 0x80) { w[0] = (char)c; return 1; }
    if (c < 0x800) { w[0] = (char)(0xC0 | c >> 6); w[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) {
        w[0] = (char)(0xE0 | c >> 12); w[1] = (char)(0x80 | ((c >> 6) & 63)); w[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    w[0] = (char)(0xF0 | c >> 18); w[1] = (char)(0x80 | ((c >> 12) & 63));
    w[2] = (char)(0x80 | ((c >> 6) & 63)); w[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* Scans the string starting at the opening quote p[*pos]; appends the
 * token. Returns 0 on error (message in *what). */
static int scan_string(const uint8_t *p, int64_t n, int64_t *pos, zell_slice *res, const char **what) {
    int64_t i = *pos + 1;
    /* The unescaped text is never longer than the escaped one. */
    char *buf = zell_alloc(n - *pos + 1);
    int64_t w = 0;
    buf[w++] = 's';
    for (;;) {
        if (i >= n) { *what = "unterminated string"; zell_free(buf); *pos = i; return 0; }
        uint8_t c = p[i];
        if (c == '"') { i++; break; }
        if (c < 0x20) { *what = "control character in string"; zell_free(buf); *pos = i; return 0; }
        if (c != '\\') { buf[w++] = (char)c; i++; continue; }
        if (i + 1 >= n) { *what = "unterminated string"; zell_free(buf); *pos = i; return 0; }
        uint8_t e = p[i + 1];
        i += 2;
        switch (e) {
            case '"': buf[w++] = '"'; break;
            case '\\': buf[w++] = '\\'; break;
            case '/': buf[w++] = '/'; break;
            case 'b': buf[w++] = '\b'; break;
            case 'f': buf[w++] = '\f'; break;
            case 'n': buf[w++] = '\n'; break;
            case 'r': buf[w++] = '\r'; break;
            case 't': buf[w++] = '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!read_hex4(p, n, i, &cp)) { *what = "invalid \\u escape"; zell_free(buf); *pos = i; return 0; }
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    uint32_t lo;
                    if (i + 6 <= n && p[i] == '\\' && p[i + 1] == 'u' && read_hex4(p, n, i + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                w += put_utf8(buf + w, cp);
                break;
            }
            default:
                *what = "invalid escape in string";
                zell_free(buf);
                *pos = i - 2;
                return 0;
        }
    }
    push_token(res, buf, w);
    zell_free(buf);
    *pos = i;
    return 1;
}

static inline int is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

void zell_encoding_json_scan(const zell_string *text, zell_slice *out, zell_error **err) {
    memset(out, 0, sizeof *out);
    if (err) *err = NULL;
    const uint8_t *p = (const uint8_t *)(text ? text->data : "");
    int64_t n = text ? text->len : 0;
    zell_slice res;
    zell_slice_new_ops(&res, 8, 0, 16, &zell_ops_ref);
    int64_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',') {
            push_token(&res, (const char *)p + i, 1);
            i++;
            continue;
        }
        if (c == '"') {
            const char *what = NULL;
            if (!scan_string(p, n, &i, &res, &what)) { json_fail(&res, err, what, i); return; }
            continue;
        }
        if (c == '-' || is_digit(c)) {
            int64_t s = i;
            if (p[i] == '-') i++;
            if (i >= n || !is_digit(p[i])) { json_fail(&res, err, "invalid number", s); return; }
            if (p[i] == '0') i++;
            else while (i < n && is_digit(p[i])) i++;
            if (i < n && p[i] == '.') {
                i++;
                if (i >= n || !is_digit(p[i])) { json_fail(&res, err, "invalid number", s); return; }
                while (i < n && is_digit(p[i])) i++;
            }
            if (i < n && (p[i] == 'e' || p[i] == 'E')) {
                i++;
                if (i < n && (p[i] == '+' || p[i] == '-')) i++;
                if (i >= n || !is_digit(p[i])) { json_fail(&res, err, "invalid number", s); return; }
                while (i < n && is_digit(p[i])) i++;
            }
            char *buf = zell_alloc(i - s + 1);
            buf[0] = 'n';
            memcpy(buf + 1, p + s, (size_t)(i - s));
            push_token(&res, buf, i - s + 1);
            zell_free(buf);
            continue;
        }
        static const struct { const char *word; char tag; } lits[] = { { "true", 't' }, { "false", 'f' }, { "null", 'z' } };
        int matched = 0;
        for (size_t k = 0; k < sizeof lits / sizeof lits[0]; k++) {
            size_t len = strlen(lits[k].word);
            if ((int64_t)len <= n - i && memcmp(p + i, lits[k].word, len) == 0) {
                push_token(&res, &lits[k].tag, 1);
                i += (int64_t)len;
                matched = 1;
                break;
            }
        }
        if (matched) continue;
        char what[48];
        if (c >= 0x20 && c < 0x7F) snprintf(what, sizeof what, "invalid character '%c'", c);
        else snprintf(what, sizeof what, "invalid byte 0x%02x", c);
        json_fail(&res, err, what, i);
        return;
    }
    *out = res;
}

zell_string *zell_encoding_json_quote(const zell_string *s) {
    const uint8_t *p = (const uint8_t *)(s ? s->data : "");
    int64_t n = s ? s->len : 0;
    int64_t len = 2;
    for (int64_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f') len += 2;
        else if (c < 0x20) len += 6;
        else len += 1;
    }
    zell_string *r = zell__string_alloc(len);
    char *w = r->data;
    *w++ = '"';
    for (int64_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        switch (c) {
            case '"': *w++ = '\\'; *w++ = '"'; break;
            case '\\': *w++ = '\\'; *w++ = '\\'; break;
            case '\n': *w++ = '\\'; *w++ = 'n'; break;
            case '\r': *w++ = '\\'; *w++ = 'r'; break;
            case '\t': *w++ = '\\'; *w++ = 't'; break;
            case '\b': *w++ = '\\'; *w++ = 'b'; break;
            case '\f': *w++ = '\\'; *w++ = 'f'; break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    *w++ = '\\'; *w++ = 'u'; *w++ = '0'; *w++ = '0';
                    *w++ = hex[c >> 4]; *w++ = hex[c & 15];
                } else {
                    *w++ = (char)c;
                }
        }
    }
    *w++ = '"';
    *w = '\0';
    return r;
}
