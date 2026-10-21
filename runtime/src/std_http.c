/* std_http.c - hot loops of std.net.http, std.net.ws and the HPACK coder
 * (runtime/ABI.md 16.5): HTTP/1 head scanning and parsing straight from the
 * connection's read buffer (no per-line strings), header-name
 * canonicalisation with interned common names, response-head formatting
 * into one exact-size string with a per-thread cached Date, comma-token
 * search, HPACK Huffman (RFC 7541 appendix B) and WebSocket masking.
 * Everything here is a pure function of its arguments (plus the per-thread
 * date cache); the protocol state machines stay in Slang. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- interned strings ------------------------------------------------- */

#define IMM(name, lit) SLANG_STATIC_STRING(name, lit)

IMM(s_get, "GET");
IMM(s_post, "POST");
IMM(s_put, "PUT");
IMM(s_delete, "DELETE");
IMM(s_head, "HEAD");
IMM(s_options, "OPTIONS");
IMM(s_patch, "PATCH");
IMM(s_connect, "CONNECT");
IMM(s_trace, "TRACE");
IMM(s_http11, "HTTP/1.1");
IMM(s_http10, "HTTP/1.0");
IMM(s_slash, "/");

/* Common canonical header names (Go's commonHeader plus the ones the
 * library itself asks for). Returned without allocating. */
static const char *const common_names[] = {
    "A-Im", "Accept", "Accept-Charset", "Accept-Datetime", "Accept-Encoding", "Accept-Language", "Accept-Ranges",
    "Access-Control-Allow-Credentials", "Access-Control-Allow-Headers", "Access-Control-Allow-Methods",
    "Access-Control-Allow-Origin", "Access-Control-Expose-Headers", "Access-Control-Max-Age",
    "Access-Control-Request-Headers", "Access-Control-Request-Method", "Age", "Allow", "Alt-Svc", "Authorization",
    "Cache-Control", "Cc", "Connection", "Content-Disposition", "Content-Encoding", "Content-Id", "Content-Language",
    "Content-Length", "Content-Location", "Content-Range", "Content-Security-Policy", "Content-Transfer-Encoding",
    "Content-Type", "Cookie", "Date", "Dnt", "Etag", "Expect", "Expires", "Forwarded", "From", "Host",
    "Http2-Settings", "If-Match", "If-Modified-Since", "If-None-Match", "If-Range", "If-Unmodified-Since",
    "Keep-Alive", "Last-Event-Id", "Last-Modified", "Link", "Location", "Max-Forwards", "Origin", "Pragma",
    "Proxy-Authenticate", "Proxy-Authorization", "Proxy-Connection", "Range", "Referer", "Refresh", "Retry-After",
    "Sec-Websocket-Accept", "Sec-Websocket-Extensions", "Sec-Websocket-Key", "Sec-Websocket-Protocol",
    "Sec-Websocket-Version", "Server", "Set-Cookie", "Strict-Transport-Security", "Te", "Traceparent", "Tracestate",
    "Trailer", "Transfer-Encoding", "Upgrade", "Upgrade-Insecure-Requests", "User-Agent", "Vary", "Via",
    "Www-Authenticate", "X-Content-Type-Options", "X-Forwarded-For", "X-Forwarded-Host", "X-Forwarded-Proto",
    "X-Frame-Options", "X-Powered-By", "X-Real-Ip", "X-Request-Id", "X-Requested-With", "X-Xss-Protection",
};
#define NCOMMON (sizeof common_names / sizeof common_names[0])

static slang_string *common_strs[NCOMMON];
static slang__plat_once common_once = SLANG_PLAT_ONCE_INIT;

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void common_init(void) {
    const char *sorted[NCOMMON];
    memcpy(sorted, common_names, sizeof sorted);
    qsort(sorted, NCOMMON, sizeof sorted[0], cmp_names);
    for (size_t i = 0; i < NCOMMON; i++)
        common_strs[i] = slang_string_literal(sorted[i], (int64_t)strlen(sorted[i]));
}

/* The interned string equal to b[0..n), or NULL. */
static slang_string *common_lookup(const char *b, size_t n) {
    slang__plat_once_run(&common_once, common_init);
    size_t lo = 0, hi = NCOMMON;
    while (lo < hi) { /* the table is sorted (strcmp order) */
        size_t mid = (lo + hi) / 2;
        const slang_string *s = common_strs[mid];
        size_t m = (size_t)s->len;
        int c = memcmp(b, s->data, n < m ? n : m);
        if (c == 0) c = n < m ? -1 : (n > m ? 1 : 0);
        if (c == 0) return common_strs[mid];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* RFC 9110 token characters. */
static const unsigned char tchar[256] = {
    ['!'] = 1, ['#'] = 1, ['$'] = 1, ['%'] = 1, ['&'] = 1, ['\''] = 1, ['*'] = 1, ['+'] = 1, ['-'] = 1, ['.'] = 1,
    ['^'] = 1, ['_'] = 1, ['`'] = 1, ['|'] = 1, ['~'] = 1,
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1, ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
    ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1, ['E'] = 1, ['F'] = 1, ['G'] = 1, ['H'] = 1, ['I'] = 1, ['J'] = 1,
    ['K'] = 1, ['L'] = 1, ['M'] = 1, ['N'] = 1, ['O'] = 1, ['P'] = 1, ['Q'] = 1, ['R'] = 1, ['S'] = 1, ['T'] = 1,
    ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1, ['Y'] = 1, ['Z'] = 1,
    ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1, ['e'] = 1, ['f'] = 1, ['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1,
    ['k'] = 1, ['l'] = 1, ['m'] = 1, ['n'] = 1, ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1, ['s'] = 1, ['t'] = 1,
    ['u'] = 1, ['v'] = 1, ['w'] = 1, ['x'] = 1, ['y'] = 1, ['z'] = 1,
};

static int is_tchar(unsigned char c) { return tchar[c]; }

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static char low(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Canonicalises b[0..n) into d (n bytes): upper case at the start and after
 * '-', lower case elsewhere. Returns 0 if b is not a token (copied as is). */
static int canon_into(char *d, const char *b, size_t n) {
    int tok = n > 0;
    int start = 1;
    for (size_t i = 0; i < n; i++) {
        char c = b[i];
        if (!is_tchar((unsigned char)c)) tok = 0;
        d[i] = start ? up(c) : low(c);
        start = c == '-';
    }
    if (!tok) memcpy(d, b, n);
    return tok;
}

/* A string for the canonical form of b[0..n) (interned when common). */
static slang_string *canon_string(const char *b, size_t n) {
    char tmp[64];
    if (n <= sizeof tmp) {
        canon_into(tmp, b, n);
        slang_string *s = common_lookup(tmp, n);
        if (s) return s;
        return slang_string_new(tmp, (int64_t)n);
    }
    slang_string *s = slang__string_alloc_uninit((int64_t)n);
    canon_into(s->data, b, n);
    return s;
}

static int is_canonical(const char *b, size_t n) {
    int start = 1;
    for (size_t i = 0; i < n; i++) {
        char c = b[i];
        if (!is_tchar((unsigned char)c)) return 1; /* not a token: kept as is */
        if (start ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z')) return 0;
        start = c == '-';
    }
    return 1;
}

/* rtCanonical(s) string: CanonicalHeader without allocating when s is
 * already canonical (s itself, retained) or a common name. Surrounding
 * spaces and tabs are trimmed first. */
slang_string *slang_net_http_rt_canonical(const slang_string *s) {
    const char *b = slang_string_data(s);
    size_t n = (size_t)slang_string_len(s);
    size_t lo = 0, hi = n;
    while (lo < hi && (b[lo] == ' ' || b[lo] == '\t')) lo++;
    while (hi > lo && (b[hi - 1] == ' ' || b[hi - 1] == '\t')) hi--;
    if (lo == 0 && hi == n && is_canonical(b, n)) {
        slang_retain((void *)s);
        return (slang_string *)s;
    }
    return canon_string(b + lo, hi - lo);
}

/* ---- head scanning ------------------------------------------------------ */

/* rtHeadEnd(buf, from, to) int: the offset just past the blank line that
 * ends a header block in buf[from..to) ("\r\n\r\n" or "\n\n", also
 * "\n\r\n"), or -1. `from` may point anywhere before the terminator. */
int64_t slang_net_http_rt_head_end(const slang_slice *buf, int64_t from, int64_t to) {
    if (!buf || to > buf->len) to = buf ? buf->len : 0;
    if (from < 0) from = 0;
    const char *p = (const char *)(buf ? buf->data : NULL);
    int64_t i = from;
    while (i < to) {
        const char *nl = memchr(p + i, '\n', (size_t)(to - i));
        if (!nl) return -1;
        int64_t j = nl - p + 1;
        if (j < to && p[j] == '\n') return j + 1;
        if (j + 1 < to && p[j] == '\r' && p[j + 1] == '\n') return j + 2;
        if (j >= to || (j + 1 >= to && p[j] == '\r')) return -1;
        i = j;
    }
    return -1;
}

static slang_string *method_string(const char *b, size_t n) {
    switch (n) {
    case 3:
        if (memcmp(b, "GET", 3) == 0) return s_get;
        if (memcmp(b, "PUT", 3) == 0) return s_put;
        break;
    case 4:
        if (memcmp(b, "POST", 4) == 0) return s_post;
        if (memcmp(b, "HEAD", 4) == 0) return s_head;
        break;
    case 5:
        if (memcmp(b, "PATCH", 5) == 0) return s_patch;
        if (memcmp(b, "TRACE", 5) == 0) return s_trace;
        break;
    case 6:
        if (memcmp(b, "DELETE", 6) == 0) return s_delete;
        break;
    case 7:
        if (memcmp(b, "OPTIONS", 7) == 0) return s_options;
        if (memcmp(b, "CONNECT", 7) == 0) return s_connect;
        break;
    }
    return slang_string_new(b, (int64_t)n);
}

static slang_string *proto_string(const char *b, size_t n) {
    if (n == 8 && memcmp(b, "HTTP/1.1", 8) == 0) return s_http11;
    if (n == 8 && memcmp(b, "HTTP/1.0", 8) == 0) return s_http10;
    return slang_string_new(b, (int64_t)n);
}

static void push(slang_slice *out, slang_string *s) {
    slang_slice_append(out, 8, &slang_ops_ref, &s);
    slang_release(s);
}

static void fail(slang_error **err, const char *msg, const char *b, size_t n) {
    if (!err) return;
    char tmp[160];
    if (n > 60) n = 60;
    int k = snprintf(tmp, sizeof tmp, "%s \"", msg);
    for (size_t i = 0; i < n && k < (int)sizeof tmp - 4; i++) {
        unsigned char c = (unsigned char)b[i];
        tmp[k++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    tmp[k++] = '"';
    tmp[k] = 0;
    slang_string *m = slang_string_from_cstr(tmp);
    *err = slang_error_new(m);
    slang_release(m);
}

IMM(s_comma, ", ");
IMM(s_newline, "\n");
IMM(s_set_cookie_name, "Set-Cookie");

/* Stores name: value into h, joining repeated names with ", " (Set-Cookie
 * with "\n", since cookie values may contain commas). */
static void put_header(slang_map *h, slang_string *k, const char *v, size_t vn) {
    slang_string **slot = (slang_string **)slang_map_get_or_insert_str(h, k);
    slang_string *old = *slot;
    if (old && old->len > 0) {
        const slang_string *sep = slang_string_eq(k, s_set_cookie_name) ? s_newline : s_comma;
        int64_t on = old->len, sn = sep->len;
        slang_string *val = slang__string_alloc_uninit(on + sn + (int64_t)vn);
        memcpy(val->data, old->data, (size_t)on);
        memcpy(val->data + on, sep->data, (size_t)sn);
        memcpy(val->data + on + sn, v, vn);
        *slot = val;
        slang_release(old);
    } else {
        slang_release(old);
        *slot = vn ? slang_string_new(v, (int64_t)vn) : NULL;
    }
}

/* What rtParseHead saw, for rtHeadInfo (per thread, like rtAgain). */
enum {
    I_LENGTH = 1,      /* Content-Length present (value in rtHeadInfo() >> 16, or I_BADLENGTH) */
    I_CHUNKED = 2,     /* Transfer-Encoding: chunked */
    I_TE_OTHER = 4,    /* another Transfer-Encoding */
    I_CLOSE = 8,       /* Connection: close */
    I_KEEPALIVE = 16,  /* Connection: keep-alive */
    I_CONTINUE = 32,   /* Expect: 100-continue */
    I_TRACE = 64,      /* traceparent present */
    I_UPGRADE = 128,   /* Upgrade present */
    I_BADLENGTH = 256, /* invalid or conflicting Content-Length */
};
static __thread int64_t head_info;
static __thread int64_t head_length;

static int iprefix_eq(const char *a, size_t an, const char *lowlit, size_t ln) {
    if (an != ln) return 0;
    for (size_t i = 0; i < an; i++)
        if (low(a[i]) != lowlit[i]) return 0;
    return 1;
}

static int has_token_raw(const char *b, size_t n, const char *t, size_t tn);

static void note_header(const char *k, size_t kn, const char *v, size_t vn) {
    switch (kn) {
    case 14:
        if (iprefix_eq(k, kn, "content-length", 14)) {
            int64_t x = 0;
            int ok = vn > 0 && vn <= 18;
            for (size_t i = 0; ok && i < vn; i++) {
                if (v[i] < '0' || v[i] > '9') ok = 0;
                else x = x * 10 + (v[i] - '0');
            }
            if (!ok || ((head_info & I_LENGTH) && x != head_length)) head_info |= I_BADLENGTH;
            head_info |= I_LENGTH;
            head_length = x;
        }
        break;
    case 17:
        if (iprefix_eq(k, kn, "transfer-encoding", 17))
            head_info |= has_token_raw(v, vn, "chunked", 7) ? I_CHUNKED : I_TE_OTHER;
        break;
    case 10:
        if (iprefix_eq(k, kn, "connection", 10)) {
            if (has_token_raw(v, vn, "close", 5)) head_info |= I_CLOSE;
            if (has_token_raw(v, vn, "keep-alive", 10)) head_info |= I_KEEPALIVE;
        }
        break;
    case 6:
        if (iprefix_eq(k, kn, "expect", 6) && has_token_raw(v, vn, "100-continue", 12)) head_info |= I_CONTINUE;
        break;
    case 11:
        if (iprefix_eq(k, kn, "traceparent", 11)) head_info |= I_TRACE;
        break;
    case 7:
        if (iprefix_eq(k, kn, "upgrade", 7)) head_info |= I_UPGRADE;
        break;
    }
}

/* rtHeadInfo() int: flags of the last rtParseHead on this thread (see the
 * I_* enum) in the low 16 bits, the Content-Length above them. */
int64_t slang_net_http_rt_head_info(void) {
    return (head_info & 0xffff) | ((head_info & I_LENGTH) && !(head_info & I_BADLENGTH) ? head_length << 16 : 0);
}

/* Parses the header lines of b[0..n) (after the first line) into h. */
static int parse_fields(const char *b, size_t n, slang_map *h, slang_error **err) {
    size_t i = 0;
    while (i < n) {
        const char *nl = memchr(b + i, '\n', n - i);
        size_t e = nl ? (size_t)(nl - b) : n;
        size_t le = e;
        if (le > i && b[le - 1] == '\r') le--;
        if (le == i) break; /* the blank line */
        if (b[i] == ' ' || b[i] == '\t') {
            fail(err, "malformed header line (obsolete line folding)", b + i, le - i);
            return 0;
        }
        const char *colon = memchr(b + i, ':', le - i);
        if (!colon || colon == b + i) {
            fail(err, "malformed header line", b + i, le - i);
            return 0;
        }
        size_t kn = (size_t)(colon - (b + i));
        for (size_t j = 0; j < kn; j++)
            if (!is_tchar((unsigned char)b[i + j])) {
                fail(err, "malformed header name", b + i, kn);
                return 0;
            }
        size_t vs = (size_t)(colon - b) + 1, ve = le;
        while (vs < ve && (b[vs] == ' ' || b[vs] == '\t')) vs++;
        while (ve > vs && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
        note_header(b + i, kn, b + vs, ve - vs);
        slang_string *k = canon_string(b + i, kn);
        put_header(h, k, b + vs, ve - vs);
        slang_release(k);
        i = e + 1;
    }
    return 1;
}

/* rtParseHead(buf, start, end, kind, h) []string!: parses the header block
 * buf[start..end) (end from rtHeadEnd) into h (canonical names) and returns
 * the pieces of the first line: kind 0 (request) [method, path, query,
 * proto] (the target split at '?'; an absolute-form target keeps only its
 * path), kind 1 (response) [proto, status, reason]. Common methods,
 * protocols and header names are interned (no allocation). */
void slang_net_http_rt_parse_head(const slang_slice *buf, int64_t start, int64_t end, int64_t kind, slang_map *h,
                                  slang_slice *out, slang_error **err) {
    memset(out, 0, sizeof *out);
    if (err) *err = NULL;
    head_info = 0;
    head_length = 0;
    if (!buf || start < 0 || end > buf->len || start > end || !h) {
        fail(err, "malformed header", "", 0);
        return;
    }
    const char *b = (const char *)buf->data + start;
    size_t n = (size_t)(end - start);
    /* skip blank lines before a request line (RFC 9112 2.2) */
    while (kind == 0 && n >= 2 && b[0] == '\r' && b[1] == '\n') b += 2, n -= 2;
    while (kind == 0 && n >= 1 && b[0] == '\n') b++, n--;
    const char *nl = memchr(b, '\n', n);
    size_t le = nl ? (size_t)(nl - b) : n;
    size_t rest = nl ? le + 1 : n;
    if (le > 0 && b[le - 1] == '\r') le--;
    slang_slice res;
    slang_slice_new_ops(&res, 8, 0, 4, &slang_ops_ref);
    if (kind == 0) {
        const char *sp1 = memchr(b, ' ', le);
        const char *sp2 = sp1 ? memchr(sp1 + 1, ' ', le - (size_t)(sp1 + 1 - b)) : NULL;
        if (!sp1 || !sp2 || sp1 == b || sp2 == sp1 + 1 || memchr(sp2 + 1, ' ', le - (size_t)(sp2 + 1 - b))) {
            slang_slice_release(&res);
            fail(err, "malformed request line", b, le);
            return;
        }
        size_t mn = (size_t)(sp1 - b);
        for (size_t j = 0; j < mn; j++)
            if (!is_tchar((unsigned char)b[j])) {
                slang_slice_release(&res);
                fail(err, "malformed request line", b, le);
                return;
            }
        const char *t = sp1 + 1;
        size_t tn = (size_t)(sp2 - t);
        const char *pr = sp2 + 1;
        size_t pn = le - (size_t)(pr - b);
        if (pn != 8 || memcmp(pr, "HTTP/1.", 7) != 0 || pr[7] < '0' || pr[7] > '9') {
            slang_slice_release(&res);
            fail(err, "malformed request line", b, le);
            return;
        }
        /* absolute-form: keep the path */
        if (tn > 7 && (memcmp(t, "http://", 7) == 0 || (tn > 8 && memcmp(t, "https://", 8) == 0))) {
            const char *a = memchr(t, ':', tn) + 3;
            const char *ps = memchr(a, '/', tn - (size_t)(a - t));
            const char *qs = memchr(a, '?', tn - (size_t)(a - t));
            if (qs && (!ps || qs < ps)) ps = qs;
            if (!ps) {
                tn = 1;
                t = "/";
            } else {
                tn -= (size_t)(ps - t);
                t = ps;
            }
        }
        const char *q = memchr(t, '?', tn);
        size_t pathn = q ? (size_t)(q - t) : tn;
        push(&res, method_string(b, mn));
        push(&res, (pathn == 1 && t[0] == '/') ? s_slash : slang_string_new(t, (int64_t)pathn));
        push(&res, (q && tn - pathn > 1) ? slang_string_new(q + 1, (int64_t)(tn - pathn - 1)) : NULL);
        push(&res, proto_string(pr, pn));
    } else {
        const char *sp1 = memchr(b, ' ', le);
        if (!sp1 || le < 12 || memcmp(b, "HTTP/", 5) != 0) {
            slang_slice_release(&res);
            fail(err, "malformed status line", b, le);
            return;
        }
        const char *st = sp1 + 1;
        size_t stn = 0;
        while (st + stn < b + le && st[stn] != ' ') stn++;
        if (stn != 3 || st[0] < '1' || st[0] > '9' || st[1] < '0' || st[1] > '9' || st[2] < '0' || st[2] > '9') {
            slang_slice_release(&res);
            fail(err, "malformed status line", b, le);
            return;
        }
        const char *rs = st + stn;
        if (rs < b + le) rs++;
        push(&res, proto_string(b, (size_t)(sp1 - b)));
        push(&res, slang_string_new(st, 3));
        push(&res, slang_string_new(rs, (int64_t)(le - (size_t)(rs - b))));
    }
    if (!parse_fields(b + rest, n - rest, h, err)) {
        slang_slice_release(&res);
        return;
    }
    *out = res;
}

/* ---- comma-separated tokens ---------------------------------------------- */

/* rtHasToken(v, token) bool: whether the comma-separated list v contains
 * token (ASCII case-insensitive, surrounding spaces ignored; a token
 * followed by ";params" also counts). */
bool slang_net_http_rt_has_token(const slang_string *v, const slang_string *token) {
    return has_token_raw(slang_string_data(v), (size_t)slang_string_len(v), slang_string_data(token),
                         (size_t)slang_string_len(token));
}

static int has_token_raw(const char *b, size_t n, const char *t, size_t tn) {
    size_t i = 0;
    while (i <= n) {
        size_t e = i;
        while (e < n && b[e] != ',') e++;
        size_t s = i, f = e;
        while (s < f && (b[s] == ' ' || b[s] == '\t')) s++;
        const char *semi = memchr(b + s, ';', f - s);
        if (semi) f = (size_t)(semi - b);
        while (f > s && (b[f - 1] == ' ' || b[f - 1] == '\t')) f--;
        if (f - s == tn) {
            size_t j = 0;
            while (j < tn && low(b[s + j]) == low(t[j])) j++;
            if (j == tn) return 1;
        }
        i = e + 1;
    }
    return 0;
}

/* ---- response heads -------------------------------------------------------- */

static const char *status_text(int64_t code) {
    switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    }
    return "";
}

/* rtStatusText(code) string. */
slang_string *slang_net_http_rt_status_text(int64_t code) {
    return slang_string_from_cstr(status_text(code));
}

/* The current time as an IMF-fixdate ("Mon, 02 Jan 2006 15:04:05 GMT", 29
 * bytes), formatted at most once per second per thread. */
static const char *http_date(void) {
    static __thread time_t cached = -1;
    static __thread char buf[64];
    time_t now = time(NULL);
    if (now != cached) {
        static const char days[] = "SunMonTueWedThuFriSat";
        static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        struct tm tm;
        slang__plat_utc_tm((int64_t)now, &tm);
        snprintf(buf, sizeof buf, "%.3s, %02d %.3s %04d %02d:%02d:%02d GMT", days + 3 * tm.tm_wday, tm.tm_mday & 31,
                 mons + 3 * tm.tm_mon, (tm.tm_year + 1900) % 10000, tm.tm_hour & 31, tm.tm_min & 63, tm.tm_sec & 63);
        cached = now;
    }
    return buf;
}

/* rtDate() string: the cached Date header value. */
slang_string *slang_net_http_rt_date(void) {
    return slang_string_new(http_date(), 29);
}


/* The head is built in a per-thread scratch buffer (grown as needed), then
 * copied into one exact-size string. */
typedef struct {
    char *p;
    size_t n, cap;
} wbuf;

static __thread char *scratch;
static __thread size_t scratch_cap;

static void reserve(wbuf *w, size_t more) {
    if (w->n + more <= w->cap) return;
    size_t c = w->cap ? w->cap : 1024;
    while (c < w->n + more) c *= 2;
    w->p = realloc(w->p, c);
    if (!w->p) slang_panic("out of memory");
    w->cap = c;
}

static void put(wbuf *w, const char *s, size_t n) {
    reserve(w, n);
    memcpy(w->p + w->n, s, n);
    w->n += n;
}

/* A header value with CR, LF and NUL replaced by spaces (no injection). */
static void put_value(wbuf *w, const char *s, size_t n) {
    reserve(w, n);
    char *d = w->p + w->n;
    memcpy(d, s, n);
    if (memchr(s, '\r', n) || memchr(s, '\n', n) || memchr(s, 0, n))
        for (size_t i = 0; i < n; i++)
            if (d[i] == '\r' || d[i] == '\n' || d[i] == 0) d[i] = ' ';
    w->n += n;
}

static size_t fmt_int(char *tmp, int64_t v) {
    char r[24];
    size_t k = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        r[k++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    size_t n = 0;
    if (v < 0) tmp[n++] = '-';
    while (k) tmp[n++] = r[--k];
    return n;
}

static void put_int(wbuf *w, int64_t v) {
    char tmp[24];
    put(w, tmp, fmt_int(tmp, v));
}

/* Header names the formatter decides itself (by length, then folded). */
enum { K_OTHER, K_SKIP, K_TYPE, K_DATE, K_COOKIE };
static int key_kind(const char *k, size_t n) {
    switch (n) {
    case 4: return iprefix_eq(k, n, "date", 4) ? K_DATE : K_OTHER;
    case 10:
        if (iprefix_eq(k, n, "connection", 10)) return K_SKIP;
        return iprefix_eq(k, n, "set-cookie", 10) ? K_COOKIE : K_OTHER;
    case 12: return iprefix_eq(k, n, "content-type", 12) ? K_TYPE : K_OTHER;
    case 14: return iprefix_eq(k, n, "content-length", 14) ? K_SKIP : K_OTHER;
    case 17: return iprefix_eq(k, n, "transfer-encoding", 17) ? K_SKIP : K_OTHER;
    }
    return K_OTHER;
}

static void put_name(wbuf *w, const char *k, size_t n) {
    reserve(w, n);
    if (is_canonical(k, n)) memcpy(w->p + w->n, k, n);
    else canon_into(w->p + w->n, k, n);
    w->n += n;
}

enum {
    F_KEEPALIVE = 1, /* "Connection: keep-alive" (an HTTP/1.0 client asked for it) */
    F_CLOSE = 2,     /* "Connection: close" */
    F_HEADONLY = 4,  /* no body bytes (HEAD) */
    F_NODATE = 8,    /* no Date header */
    F_CHUNKED = 16,  /* "Transfer-Encoding: chunked" instead of Content-Length */
    F_NOLENGTH = 32, /* neither (1xx, 204, 304, a body that runs to close) */
    F_NOTYPE = 64,   /* no default Content-Type */
    F_REQUEST = 128, /* a request head: `status` unused, first line is `line` */
};

static void emit_head(wbuf *w, int64_t status, const slang_string *line, slang_map *h, int64_t blen, int64_t flags) {
    if (flags & F_REQUEST) {
        put(w, slang_string_data(line), (size_t)slang_string_len(line));
        put(w, "\r\n", 2);
    } else {
        if (status == 200) put(w, "HTTP/1.1 200 OK\r\n", 17);
        else {
            put(w, "HTTP/1.1 ", 9);
            put_int(w, status);
            put(w, " ", 1);
            const char *st = status_text(status);
            put(w, st, strlen(st));
            put(w, "\r\n", 2);
        }
    }
    int has_type = 0, has_date = 0;
    if (h) {
        slang_map_iter it;
        slang_map_iter_init(&it, h);
        void *kp, *vp;
        while (slang_map_iter_next(&it, &kp, &vp)) {
            const slang_string *k = *(slang_string **)kp;
            const slang_string *v = *(slang_string **)vp;
            const char *kb = slang_string_data(k);
            size_t kn = (size_t)slang_string_len(k);
            if (kn == 0) continue;
            int kind = key_kind(kb, kn);
            if (kind == K_SKIP) continue;
            if (kind == K_TYPE && !(flags & F_REQUEST)) has_type = 1;
            if (kind == K_DATE) has_date = 1;
            const char *vb = slang_string_data(v);
            size_t vn = (size_t)slang_string_len(v);
            /* Set-Cookie values joined by "\n" go out as separate lines */
            size_t s = 0;
            for (;;) {
                size_t e = vn;
                if (kind == K_COOKIE) {
                    const char *x = memchr(vb + s, '\n', vn - s);
                    if (x) e = (size_t)(x - vb);
                }
                put_name(w, kb, kn);
                put(w, ": ", 2);
                put_value(w, vb + s, e - s);
                put(w, "\r\n", 2);
                if (e >= vn) break;
                s = e + 1;
            }
        }
    }
    if (!has_type && !(flags & (F_NOTYPE | F_REQUEST)) && blen > 0) put(w, "Content-Type: text/plain; charset=utf-8\r\n", 41);
    if (!has_date && !(flags & (F_NODATE | F_REQUEST))) {
        put(w, "Date: ", 6);
        put(w, http_date(), 29);
        put(w, "\r\n", 2);
    }
    if (flags & F_CHUNKED) put(w, "Transfer-Encoding: chunked\r\n", 28);
    else if (!(flags & F_NOLENGTH)) {
        put(w, "Content-Length: ", 16);
        put_int(w, blen);
        put(w, "\r\n", 2);
    }
    if (flags & F_KEEPALIVE) put(w, "Connection: keep-alive\r\n", 24);
    if (flags & F_CLOSE) put(w, "Connection: close\r\n", 19);
    put(w, "\r\n", 2);
}

/* rtFormatHead(status, line, h, blen, flags, body) string: a whole
 * HTTP/1.1 message head (and `body` unless F_HEADONLY) as one string.
 * Header names are written canonically; Content-Length, Connection and
 * Transfer-Encoding from h are ignored (the flags decide them); CR/LF in
 * values become spaces. */
slang_string *slang_net_http_rt_format_head(int64_t status, const slang_string *line, slang_map *h, int64_t blen,
                                            int64_t flags, const slang_string *body) {
    wbuf w = {scratch, 0, scratch_cap};
    emit_head(&w, status, line, h, blen, flags);
    scratch = w.p;
    scratch_cap = w.cap;
    int64_t bn = (!(flags & F_HEADONLY) && body) ? slang_string_len(body) : 0;
    slang_string *s = slang__string_alloc_uninit((int64_t)w.n + bn);
    memcpy(s->data, w.p, w.n);
    if (bn) memcpy(s->data + w.n, slang_string_data(body), (size_t)bn);
    return s;
}

/* ---- WebSocket masking ------------------------------------------------------- */

/* rtMask(b, off, n, key, phase): XORs b[off..off+n) in place with the
 * 4-byte masking key (big-endian in `key`), starting at key byte `phase`. */
void slang_net_http_rt_mask(const slang_slice *b, int64_t off, int64_t n, int64_t key, int64_t phase) {
    if (!b || off < 0 || n <= 0 || off + n > b->len) return;
    unsigned char k[4] = {(unsigned char)(key >> 24), (unsigned char)(key >> 16), (unsigned char)(key >> 8),
                          (unsigned char)key};
    unsigned char *p = (unsigned char *)b->data + off;
    int64_t i = 0;
    int64_t ph = phase & 3;
    while (i < n && ((ph + i) & 3) != 0) {
        p[i] ^= k[(ph + i) & 3];
        i++;
    }
    uint32_t kw;
    memcpy(&kw, k, 4);
    uint64_t k8 = ((uint64_t)kw << 32) | kw;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        v ^= k8;
        memcpy(p + i, &v, 8);
    }
    for (; i < n; i++) p[i] ^= k[(ph + i) & 3];
}

/* rtItoa(n) string: decimal. */
slang_string *slang_net_http_rt_itoa(int64_t n) {
    char tmp[24];
    return slang_string_new(tmp, (int64_t)fmt_int(tmp, n));
}

/* rtAtoi(s) int: a non-negative decimal (no sign, no spaces, at most 18
 * digits), or -1. For Content-Length and chunk-free numbers. */
int64_t slang_net_http_rt_atoi(const slang_string *s) {
    const char *b = slang_string_data(s);
    int64_t n = slang_string_len(s);
    if (n == 0 || n > 18) return -1;
    int64_t v = 0;
    for (int64_t i = 0; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') return -1;
        v = v * 10 + (b[i] - '0');
    }
    return v;
}

/* rtHttpTime(sec) string: Unix seconds as an IMF-fixdate. */
slang_string *slang_net_http_rt_http_time(int64_t sec) {
    static const char days[] = "SunMonTueWedThuFriSat";
    static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    time_t t = (time_t)sec;
    struct tm tm;
    slang__plat_utc_tm((int64_t)t, &tm);
    char buf[64];
    int k = snprintf(buf, sizeof buf, "%.3s, %02d %.3s %04d %02d:%02d:%02d GMT", days + 3 * tm.tm_wday,
                     tm.tm_mday & 31, mons + 3 * tm.tm_mon, (tm.tm_year + 1900) % 10000, tm.tm_hour & 31,
                     tm.tm_min & 63, tm.tm_sec & 63);
    return slang_string_new(buf, k);
}

static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* rtParseHttpTime(s) int: Unix seconds of an HTTP date (IMF-fixdate,
 * RFC 850 or asctime), or -1. */
int64_t slang_net_http_rt_parse_http_time(const slang_string *s) {
    const char *b = slang_string_data(s);
    static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    char wd[16];
    if (sscanf(b, "%15[A-Za-z], %d %3s %d %d:%d:%d GMT", wd, &d, mon, &y, &hh, &mm, &ss) == 7) {
    } else if (sscanf(b, "%15[A-Za-z], %d-%3s-%d %d:%d:%d GMT", wd, &d, mon, &y, &hh, &mm, &ss) == 7) {
        y += y < 70 ? 2000 : (y < 100 ? 1900 : 0);
    } else if (sscanf(b, "%15s %3s %d %d:%d:%d %d", wd, mon, &d, &hh, &mm, &ss, &y) == 7) {
    } else {
        return -1;
    }
    const char *p = strstr(mons, mon);
    if (!p || strlen(mon) != 3 || (p - mons) % 3 != 0) return -1;
    int m = (int)(p - mons) / 3 + 1;
    if (d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60 || y < 1970) return -1;
    return days_from_civil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
}

/* ---- sentinel errors (stdlib.rs CONSTS) ---- */

#define SENTINEL(fn, text)                                                                 \
    static struct {                                                                        \
        slang_object hdr;                                                                  \
        int64_t len;                                                                       \
        char data[sizeof text];                                                            \
    } fn##_msg = {{SLANG_RC_IMMORTAL, &slang_type_string}, sizeof text - 1, text};         \
    static slang_error fn##_err = {                                                        \
        .hdr = {SLANG_RC_IMMORTAL, &slang_type_error}, .message = (slang_string *)&fn##_msg, \
    };                                                                                     \
    slang_error *fn(void);                                                                 \
    slang_error *fn(void) { return &fn##_err; }

SENTINEL(slang_net_http_err_body_too_large, "http: request body too large")
SENTINEL(slang_net_http_err_hijacked, "http: connection has been hijacked")
SENTINEL(slang_net_http_err_abort_handler, "http: abort handler")
SENTINEL(slang_net_http_err_server_closed, "http: Server closed")
SENTINEL(slang_net_http_err_handler_timeout, "http: Handler timeout")
SENTINEL(slang_net_http_err_body_closed, "http: read on closed response body")
SENTINEL(slang_net_ws_err_closed, "websocket: connection closed")
SENTINEL(slang_net_ws_err_bad_handshake, "websocket: bad handshake")

/* ---- HPACK Huffman code (RFC 7541 appendix B) -------------------------- */

static const uint32_t huff_codes[256] = {
    0x1ff8, 0x7fffd8, 0xfffffe2, 0xfffffe3, 0xfffffe4, 0xfffffe5, 0xfffffe6, 0xfffffe7,
    0xfffffe8, 0xffffea, 0x3ffffffc, 0xfffffe9, 0xfffffea, 0x3ffffffd, 0xfffffeb, 0xfffffec,
    0xfffffed, 0xfffffee, 0xfffffef, 0xffffff0, 0xffffff1, 0xffffff2, 0x3ffffffe, 0xffffff3,
    0xffffff4, 0xffffff5, 0xffffff6, 0xffffff7, 0xffffff8, 0xffffff9, 0xffffffa, 0xffffffb,
    0x14, 0x3f8, 0x3f9, 0xffa, 0x1ff9, 0x15, 0xf8, 0x7fa,
    0x3fa, 0x3fb, 0xf9, 0x7fb, 0xfa, 0x16, 0x17, 0x18,
    0x0, 0x1, 0x2, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
    0x1e, 0x1f, 0x5c, 0xfb, 0x7ffc, 0x20, 0xffb, 0x3fc,
    0x1ffa, 0x21, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
    0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72,
    0xfc, 0x73, 0xfd, 0x1ffb, 0x7fff0, 0x1ffc, 0x3ffc, 0x22,
    0x7ffd, 0x3, 0x23, 0x4, 0x24, 0x5, 0x25, 0x26,
    0x27, 0x6, 0x74, 0x75, 0x28, 0x29, 0x2a, 0x7,
    0x2b, 0x76, 0x2c, 0x8, 0x9, 0x2d, 0x77, 0x78,
    0x79, 0x7a, 0x7b, 0x7ffe, 0x7fc, 0x3ffd, 0x1ffd, 0xffffffc,
    0xfffe6, 0x3fffd2, 0xfffe7, 0xfffe8, 0x3fffd3, 0x3fffd4, 0x3fffd5, 0x7fffd9,
    0x3fffd6, 0x7fffda, 0x7fffdb, 0x7fffdc, 0x7fffdd, 0x7fffde, 0xffffeb, 0x7fffdf,
    0xffffec, 0xffffed, 0x3fffd7, 0x7fffe0, 0xffffee, 0x7fffe1, 0x7fffe2, 0x7fffe3,
    0x7fffe4, 0x1fffdc, 0x3fffd8, 0x7fffe5, 0x3fffd9, 0x7fffe6, 0x7fffe7, 0xffffef,
    0x3fffda, 0x1fffdd, 0xfffe9, 0x3fffdb, 0x3fffdc, 0x7fffe8, 0x7fffe9, 0x1fffde,
    0x7fffea, 0x3fffdd, 0x3fffde, 0xfffff0, 0x1fffdf, 0x3fffdf, 0x7fffeb, 0x7fffec,
    0x1fffe0, 0x1fffe1, 0x3fffe0, 0x1fffe2, 0x7fffed, 0x3fffe1, 0x7fffee, 0x7fffef,
    0xfffea, 0x3fffe2, 0x3fffe3, 0x3fffe4, 0x7ffff0, 0x3fffe5, 0x3fffe6, 0x7ffff1,
    0x3ffffe0, 0x3ffffe1, 0xfffeb, 0x7fff1, 0x3fffe7, 0x7ffff2, 0x3fffe8, 0x1ffffec,
    0x3ffffe2, 0x3ffffe3, 0x3ffffe4, 0x7ffffde, 0x7ffffdf, 0x3ffffe5, 0xfffff1, 0x1ffffed,
    0x7fff2, 0x1fffe3, 0x3ffffe6, 0x7ffffe0, 0x7ffffe1, 0x3ffffe7, 0x7ffffe2, 0xfffff2,
    0x1fffe4, 0x1fffe5, 0x3ffffe8, 0x3ffffe9, 0xffffffd, 0x7ffffe3, 0x7ffffe4, 0x7ffffe5,
    0xfffec, 0xfffff3, 0xfffed, 0x1fffe6, 0x3fffe9, 0x1fffe7, 0x1fffe8, 0x7ffff3,
    0x3fffea, 0x3fffeb, 0x1ffffee, 0x1ffffef, 0xfffff4, 0xfffff5, 0x3ffffea, 0x7ffff4,
    0x3ffffeb, 0x7ffffe6, 0x3ffffec, 0x3ffffed, 0x7ffffe7, 0x7ffffe8, 0x7ffffe9, 0x7ffffea,
    0x7ffffeb, 0xffffffe, 0x7ffffec, 0x7ffffed, 0x7ffffee, 0x7ffffef, 0x7fffff0, 0x3ffffee,
};
static const uint8_t huff_lens[256] = {
    13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28,
    28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    6, 10, 10, 12, 13, 6, 8, 11, 10, 10, 8, 11, 8, 6, 6, 6,
    5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 8, 15, 6, 12, 10,
    13, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 8, 13, 19, 13, 14, 6,
    15, 5, 6, 5, 6, 5, 6, 6, 6, 5, 7, 7, 6, 6, 6, 5,
    6, 7, 6, 5, 5, 6, 7, 7, 7, 7, 7, 15, 11, 14, 13, 28,
    20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23,
    24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
    22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23,
    21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
    26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25,
    19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27,
    20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
    26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26,
};

/* Decoding trie: node i has children trie[i][0/1]; a value >= 0x1000 is a
 * leaf for symbol value - 0x1000 (256 = EOS). */
static uint16_t trie[512][2];
static int trie_nodes;
static slang__plat_once trie_once = SLANG_PLAT_ONCE_INIT;

static void trie_add(uint32_t code, int len, int sym) {
    int n = 0;
    for (int i = len - 1; i >= 0; i--) {
        int bit = (code >> i) & 1;
        if (i == 0) {
            trie[n][bit] = (uint16_t)(0x1000 + sym);
        } else {
            if (trie[n][bit] == 0) trie[n][bit] = (uint16_t)++trie_nodes;
            n = trie[n][bit];
        }
    }
}

static void trie_init(void) {
    for (int s = 0; s < 256; s++) trie_add(huff_codes[s], huff_lens[s], s);
    trie_add(0x3fffffff, 30, 256);
}

/* rtHuffLen(s) int: the Huffman-coded length of s in bytes. */
int64_t slang_net_http_hpack_rt_huff_len(const slang_string *s) {
    const unsigned char *b = (const unsigned char *)slang_string_data(s);
    int64_t n = slang_string_len(s), bits = 0;
    for (int64_t i = 0; i < n; i++) bits += huff_lens[b[i]];
    return (bits + 7) / 8;
}

/* rtHuffEncode(s) []byte: s Huffman-coded, padded with the EOS prefix. */
void slang_net_http_hpack_rt_huff_encode(const slang_string *s, slang_slice *out) {
    const unsigned char *b = (const unsigned char *)slang_string_data(s);
    int64_t n = slang_string_len(s);
    int64_t len = slang_net_http_hpack_rt_huff_len(s);
    memset(out, 0, sizeof *out);
    if (len == 0) return;
    slang_slice_new(out, 1, len, len);
    unsigned char *d = out->data;
    uint64_t acc = 0;
    int nb = 0;
    int64_t k = 0;
    for (int64_t i = 0; i < n; i++) {
        acc = (acc << huff_lens[b[i]]) | huff_codes[b[i]];
        nb += huff_lens[b[i]];
        while (nb >= 8) {
            nb -= 8;
            d[k++] = (unsigned char)(acc >> nb);
        }
        acc &= (((uint64_t)1) << nb) - 1;
    }
    if (nb > 0) d[k++] = (unsigned char)((acc << (8 - nb)) | (0xff >> nb));
}

/* rtHuffDecode(b, off, n) string!: decodes b[off..off+n). Fails on EOS in
 * the data, padding longer than 7 bits or not all ones. */
slang_string *slang_net_http_hpack_rt_huff_decode(const slang_slice *b, int64_t off, int64_t n, slang_error **err) {
    if (err) *err = NULL;
    slang__plat_once_run(&trie_once, trie_init);
    if (!b || off < 0 || n < 0 || off + n > b->len) {
        if (err) *err = slang_error_new_cstr("hpack: invalid Huffman range");
        return NULL;
    }
    const unsigned char *p = (const unsigned char *)b->data + off;
    /* each symbol is at least 5 bits: n * 8 / 5 bytes at most */
    size_t cap = (size_t)n * 8 / 5 + 1;
    char stackbuf[256];
    char *d = cap <= sizeof stackbuf ? stackbuf : malloc(cap);
    if (!d) slang_panic("out of memory");
    size_t k = 0;
    int node = 0, depth = 0, ones = 1;
    for (int64_t i = 0; i < n; i++) {
        for (int j = 7; j >= 0; j--) {
            int bit = (p[i] >> j) & 1;
            int next = trie[node][bit];
            depth++;
            ones = ones && bit;
            if (next >= 0x1000) {
                int sym = next - 0x1000;
                if (sym == 256) goto bad;
                d[k++] = (char)sym;
                node = 0;
                depth = 0;
                ones = 1;
            } else if (next == 0) {
                goto bad;
            } else {
                node = next;
            }
        }
    }
    if (depth > 7 || !ones) goto bad;
    slang_string *s = slang_string_new(d, (int64_t)k);
    if (d != stackbuf) free(d);
    return s;
bad:
    if (d != stackbuf) free(d);
    if (err) *err = slang_error_new_cstr("hpack: invalid Huffman-encoded data");
    return NULL;
}
