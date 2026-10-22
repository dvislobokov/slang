/* std_fmt_sprintf.c - std.fmt.Sprintf (zell_fmt_sprintf): Go's fmt verbs.
 *
 * The compiler lowers every argument i (evaluated once) to its kind
 * kinds[i] ('i' signed integer, 'u' unsigned integer, 'f' float, 'g'
 * float32, 'c' char, 'b' bool, 's' string, 'o' any other type) and one value:
 * ints[i] for integers and chars (unsigned ones as their bit pattern),
 * floats[i] for floats, texts[i] for the others (a string itself, or the
 * value formatted as println would print it); unused slots are 0 / "". Supported: %v %s %d %f %F %e %E %g %G %x %X %o %b
 * %c %q %t %%, flags '-' '0' '+' ' ' '#', width and precision. A verb that
 * does not fit its argument prints like Go: %!d(string=hi); a missing argument
 * prints %!d(MISSING). */
#include "rt_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t len, cap; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->len + n + 1) cap *= 2;
        b->p = zell_realloc(b->p, (int64_t)cap);
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void sb_ch(sbuf *b, char c) { sb_put(b, &c, 1); }

typedef struct { bool minus, zero, plus, space, sharp; int width, prec; } spec;

/* Appends s[0..n) padded to the spec's width. Zero padding goes after a sign
 * or 0x prefix when `numeric`. */
static void pad(sbuf *b, const spec *sp, const char *s, size_t n, bool numeric) {
    size_t w = sp->width > 0 ? (size_t)sp->width : 0;
    if (n >= w) { sb_put(b, s, n); return; }
    size_t fill = w - n;
    if (sp->minus) { sb_put(b, s, n); while (fill--) sb_ch(b, ' '); return; }
    if (sp->zero && numeric) {
        size_t pre = 0;
        if (n && (s[0] == '-' || s[0] == '+' || s[0] == ' ')) pre = 1;
        if (n >= pre + 2 && s[pre] == '0' && (s[pre + 1] == 'x' || s[pre + 1] == 'X')) pre += 2;
        sb_put(b, s, pre);
        while (fill--) sb_ch(b, '0');
        sb_put(b, s + pre, n - pre);
        return;
    }
    while (fill--) sb_ch(b, ' ');
    sb_put(b, s, n);
}

static void fmt_int(sbuf *b, const spec *sp, char verb, int64_t v, bool is_unsigned) {
    char digits[80];
    uint64_t u = is_unsigned ? (uint64_t)v : (v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v);
    bool neg = !is_unsigned && v < 0;
    int base = verb == 'x' || verb == 'X' ? 16 : verb == 'o' ? 8 : verb == 'b' ? 2 : 10;
    const char *dig = verb == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { digits[n++] = dig[u % (uint64_t)base]; u /= (uint64_t)base; } while (u);
    while (sp->prec >= 0 && n < sp->prec && n < 64) digits[n++] = '0';
    char out[100];
    int k = 0;
    if (neg) out[k++] = '-';
    else if (sp->plus) out[k++] = '+';
    else if (sp->space) out[k++] = ' ';
    if (sp->sharp && base == 16) { out[k++] = '0'; out[k++] = verb; }
    if (sp->sharp && base == 8) out[k++] = '0';
    while (n) out[k++] = digits[--n];
    pad(b, sp, out, (size_t)k, sp->prec < 0);
}

static void fmt_float(sbuf *b, const spec *sp, char verb, double d) {
    char f[16], out[512];
    int k = 0;
    f[k++] = '%';
    if (sp->plus) f[k++] = '+';
    else if (sp->space) f[k++] = ' ';
    if (sp->sharp) f[k++] = '#';
    f[k++] = '.';
    f[k++] = '*';
    f[k++] = verb == 'F' ? 'f' : verb;
    f[k] = 0;
    int prec = sp->prec >= 0 ? sp->prec : 6;
    int n;
    if ((verb == 'g' || verb == 'G') && sp->prec < 0) {
        /* shortest representation, like %v */
        char tmp[64];
        n = zell__format_float(tmp, sizeof tmp, d);
        if (sp->plus && d >= 0) { out[0] = '+'; memcpy(out + 1, tmp, (size_t)n); n++; }
        else memcpy(out, tmp, (size_t)n);
    } else {
        n = snprintf(out, sizeof out, f, prec, d);
        if (n < 0) n = 0;
        if (n >= (int)sizeof out) n = (int)sizeof out - 1;
    }
    pad(b, sp, out, (size_t)n, true);
}

static void fmt_quote(sbuf *b, const spec *sp, const char *s, size_t n) {
    sbuf q = { 0 };
    sb_ch(&q, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': sb_put(&q, "\\\"", 2); break;
        case '\\': sb_put(&q, "\\\\", 2); break;
        case '\n': sb_put(&q, "\\n", 2); break;
        case '\t': sb_put(&q, "\\t", 2); break;
        case '\r': sb_put(&q, "\\r", 2); break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char e[8];
                snprintf(e, sizeof e, "\\x%02x", c);
                sb_put(&q, e, 4);
            } else {
                sb_ch(&q, (char)c);
            }
        }
    }
    sb_ch(&q, '"');
    pad(b, sp, q.p, q.len, false);
    zell_free(q.p);
}

/* The %v text of an argument (as println prints it). Owned. */
static zell_string *arg_text(char k, int64_t ival, double fval, const zell_string *text) {
    char buf[64];
    switch (k) {
    case 'i': snprintf(buf, sizeof buf, "%" PRId64, ival); break;
    case 'u': snprintf(buf, sizeof buf, "%" PRIu64, (uint64_t)ival); break;
    case 'f': zell__format_float(buf, sizeof buf, fval); break;
    case 'g': zell__format_float(buf, sizeof buf, zell_float32_shortest((float)fval)); break;
    case 'c': buf[zell__utf8_encode(buf, (int32_t)ival)] = 0; break;
    default: zell_retain((void *)text); return (zell_string *)text;
    }
    return zell_string_from_cstr(buf);
}

static const char *kind_name(char k) {
    switch (k) {
    case 'i': return "int";
    case 'u': return "uint";
    case 'f': return "float";
    case 'g': return "float32";
    case 'c': return "char";
    case 'b': return "bool";
    case 's': return "string";
    default: return "value";
    }
}

static void bad_verb(sbuf *b, char verb, char kind, const zell_string *text) {
    char head[16];
    int n = snprintf(head, sizeof head, "%%!%c(", verb);
    sb_put(b, head, (size_t)n);
    const char *kn = kind_name(kind);
    sb_put(b, kn, strlen(kn));
    sb_ch(b, '=');
    sb_put(b, zell_string_data(text), (size_t)zell_string_len(text));
    sb_ch(b, ')');
}

static bool is_float(char k) { return k == 'f' || k == 'g'; }

zell_string *zell_fmt_sprintf(const zell_string *format, const zell_string *kinds, const zell_slice *ints,
                                const zell_slice *floats, const zell_slice *texts) {
    const char *f = zell_string_data(format);
    int64_t n = zell_string_len(format);
    const char *kd = zell_string_data(kinds);
    int64_t nargs = zell_string_len(kinds);
    const int64_t *iv = ints && ints->len ? ints->data : NULL;
    const double *fv = floats && floats->len ? floats->data : NULL;
    zell_string *const *tv = texts && texts->len ? texts->data : NULL;
    sbuf b = { 0 };
    int64_t arg = 0;
    for (int64_t i = 0; i < n;) {
        if (f[i] != '%') { sb_ch(&b, f[i++]); continue; }
        i++;
        spec sp = { false, false, false, false, false, 0, -1 };
        for (; i < n; i++) {
            if (f[i] == '-') sp.minus = true;
            else if (f[i] == '0') sp.zero = true;
            else if (f[i] == '+') sp.plus = true;
            else if (f[i] == ' ') sp.space = true;
            else if (f[i] == '#') sp.sharp = true;
            else break;
        }
        while (i < n && f[i] >= '0' && f[i] <= '9') { if (sp.width < 100000) sp.width = sp.width * 10 + (f[i] - '0'); i++; }
        if (i < n && f[i] == '.') {
            i++;
            sp.prec = 0;
            while (i < n && f[i] >= '0' && f[i] <= '9') { if (sp.prec < 100000) sp.prec = sp.prec * 10 + (f[i] - '0'); i++; }
        }
        if (i >= n) { sb_put(&b, "%!(NOVERB)", 10); break; }
        char verb = f[i++];
        if (verb == '%') { sb_ch(&b, '%'); continue; }
        if (arg >= nargs) {
            char m[24];
            int l = snprintf(m, sizeof m, "%%!%c(MISSING)", verb);
            sb_put(&b, m, (size_t)l);
            continue;
        }
        char k = kd[arg];
        int64_t ival = iv ? iv[arg] : 0;
        double fval = fv ? fv[arg] : 0;
        zell_string *text = arg_text(k, ival, fval, tv ? tv[arg] : NULL);
        arg++;
        switch (verb) {
        case 'v':
            if (is_float(k) && (sp.prec >= 0 || sp.plus)) fmt_float(&b, &sp, 'g', fval);
            else if ((k == 'i' || k == 'u') && (sp.plus || sp.prec >= 0)) fmt_int(&b, &sp, 'd', ival, k == 'u');
            else pad(&b, &sp, zell_string_data(text), (size_t)zell_string_len(text), k == 'i' || k == 'u' || is_float(k));
            break;
        case 's':
            if (k == 'i' || k == 'u' || is_float(k) || k == 'c' || k == 'b') bad_verb(&b, verb, k, text);
            else {
                size_t l = (size_t)zell_string_len(text);
                if (sp.prec >= 0 && (size_t)sp.prec < l) l = (size_t)sp.prec;
                pad(&b, &sp, zell_string_data(text), l, false);
            }
            break;
        case 'd': case 'o': case 'b':
            if (k == 'i' || k == 'u' || (k == 'c' && verb != 'b')) fmt_int(&b, &sp, verb, ival, k == 'u');
            else if (is_float(k) && verb == 'b') fmt_float(&b, &sp, 'a', fval);
            else bad_verb(&b, verb, k, text);
            break;
        case 'x': case 'X':
            if (k == 'i' || k == 'u' || k == 'c') fmt_int(&b, &sp, verb, ival, k == 'u');
            else if (k == 's') {
                const char *dig = verb == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
                sbuf h = { 0 };
                const unsigned char *s = (const unsigned char *)zell_string_data(text);
                for (int64_t j = 0; j < zell_string_len(text); j++) { sb_ch(&h, dig[s[j] >> 4]); sb_ch(&h, dig[s[j] & 15]); }
                pad(&b, &sp, h.p ? h.p : "", h.len, false);
                zell_free(h.p);
            } else if (is_float(k)) fmt_float(&b, &sp, verb == 'x' ? 'a' : 'A', fval);
            else bad_verb(&b, verb, k, text);
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            if (is_float(k)) fmt_float(&b, &sp, verb, fval);
            else bad_verb(&b, verb, k, text);
            break;
        case 'c':
            if (k == 'c' || k == 'i' || k == 'u') {
                char u8[4];
                int l = zell__utf8_encode(u8, (int32_t)ival);
                pad(&b, &sp, u8, (size_t)l, false);
            } else bad_verb(&b, verb, k, text);
            break;
        case 'q':
            if (k == 's') fmt_quote(&b, &sp, zell_string_data(text), (size_t)zell_string_len(text));
            else if (k == 'c') {
                char u8[8] = "'";
                int l = zell__utf8_encode(u8 + 1, (int32_t)ival);
                u8[l + 1] = '\'';
                pad(&b, &sp, u8, (size_t)l + 2, false);
            } else bad_verb(&b, verb, k, text);
            break;
        case 't':
            if (k == 'b') pad(&b, &sp, zell_string_data(text), (size_t)zell_string_len(text), false);
            else bad_verb(&b, verb, k, text);
            break;
        default: {
            char m[8];
            int l = snprintf(m, sizeof m, "%%!%c(", verb);
            sb_put(&b, m, (size_t)l);
            sb_put(&b, zell_string_data(text), (size_t)zell_string_len(text));
            sb_ch(&b, ')');
        }
        }
        zell_release(text);
    }
    if (arg < nargs) {
        sb_put(&b, "%!(EXTRA ", 9);
        for (int64_t j = arg; j < nargs; j++) {
            if (j > arg) sb_put(&b, ", ", 2);
            const char *kn = kind_name(kd[j]);
            sb_put(&b, kn, strlen(kn));
            sb_ch(&b, '=');
            zell_string *t = arg_text(kd[j], iv ? iv[j] : 0, fv ? fv[j] : 0, tv ? tv[j] : NULL);
            sb_put(&b, zell_string_data(t), (size_t)zell_string_len(t));
            zell_release(t);
        }
        sb_ch(&b, ')');
    }
    zell_string *r = zell_string_new(b.p ? b.p : "", (int64_t)b.len);
    zell_free(b.p);
    return r;
}
